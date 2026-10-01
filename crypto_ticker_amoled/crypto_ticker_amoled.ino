/*
  Crypto Ticker - LilyGO T-Display-S3 AMOLED 1.91" (non-touch, 536x240)
  ---------------------------------------------------------------------
  Smooth scrolling "stock exchange" style ticker of crypto prices in AUD.
  Data: CoinGecko public API (no key needed, refreshed every 60 s).

  v2 features:
    - AUD pricing
    - Burn-in protection: pixel shift, night dimming, pause auto-resume
    - Button (BOOT / GPIO0) toggles pause

  v3: uses the LilyGO rm67162 driver files in this folder (same as
  BTCDisplay) instead of the LilyGo AMOLED library.

  v1.1.0: Wi-Fi is set up from a phone - no Wi-Fi details in the code.
    On first start (or after holding the button 5 s) the ticker creates
    a "CryptoTicker-XXXX" network and shows a QR code. Join it from a
    phone, pick your home Wi-Fi, enter the password and tap Save.

  v1.2.0: settings page. Open http://cryptoticker.local (or the IP shown
    on screen) on any phone or computer on the same Wi-Fi to add/remove
    coins and change currency, speed, brightness, night hours and time zone.

  Build requirements:
    - "esp32 by Espressif Systems" board package v2.0.17
    - TFT_eSPI (Bodmer, 2.5.43) - used only for the off-screen sprite
    - ArduinoJson (v7.x)
    - WiFiManager (tzapu, v2.0.17)
    - pins_config.h, rm67162.h/.cpp, qrcode_rm.h/.c, FreeSansBold36pt7b.h
      in the same folder

  Board settings:
    ESP32S3 Dev Module, USB CDC On Boot: Enabled, Flash Size: 16MB,
    PSRAM: OPI PSRAM, Partition: 16M Flash (3MB APP/9.9MB FATFS)
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include "qrcode_rm.h"
#include "rm67162.h"
#include "FreeSansBold36pt7b.h"   // 50% larger than the 24pt font

#ifndef BOARD_HAS_PSRAM
#error "Set PSRAM: OPI PSRAM in the Tools menu"
#endif
#include <time.h>

#define FW_VERSION "1.2.0"   // see CHANGELOG.md

// ================== DEFAULT SETTINGS ==================
// Everything here can be changed from the settings page on your phone or
// computer: http://cryptoticker.local  (or the IP address shown on screen).
// These values are only the factory defaults.
#define MAX_COINS 20
#define MDNS_NAME "cryptoticker"

char     CURRENCY[8]      = "aud";
char     CURRENCY_SYM[8]  = "$";
char     TZ_INFO[64]      = "AEST-10AEDT,M10.1.0,M4.1.0/3";  // Sydney/Melbourne
bool     SHOW_FEAR_GREED  = true;
float    SCROLL_SPEED_PPS = 110.0;  // pixels per second
uint8_t  BRIGHTNESS_DAY   = 180;    // 0-255
uint8_t  BRIGHTNESS_NIGHT = 35;     // 0-255 (0 = screen effectively off)
int      NIGHT_START_HOUR = 23;     // dim from 11 pm...
int      NIGHT_END_HOUR   = 7;      // ...until 7 am

struct CoinDef { const char* id; const char* symbol; };
const CoinDef DEFAULT_COINS[] = {
  {"bitcoin",  "BTC"},
  {"ethereum", "ETH"},
  {"solana",   "SOL"},
  {"ripple",   "XRP"},
  {"cardano",  "ADA"},
  {"dogecoin", "DOGE"},
};
const int NUM_DEFAULT_COINS = sizeof(DEFAULT_COINS) / sizeof(DEFAULT_COINS[0]);

// ---- Fixed timings ----
const uint32_t FETCH_INTERVAL_MS = 60000;   // 60 s keeps well inside free rate limits
const uint32_t FNG_INTERVAL_MS   = 1800000; // Fear & Greed updates daily; check every 30 min
const uint32_t PIXEL_SHIFT_MS    = 120000;  // move static elements every 2 min
const uint32_t PAUSE_TIMEOUT_MS  = 300000;  // auto-resume after 5 min paused

// ---- Button ----
const int      BUTTON_PIN        = PIN_BUTTON_1;  // BOOT button (GPIO0), active LOW
const uint32_t WIFI_RESET_HOLD_MS = 5000;        // hold this long to re-run Wi-Fi setup
const uint32_t SETUP_TIMEOUT_S    = 600;         // setup screen gives up after 10 min
// ======================================================

struct Coin {
  char  id[40];      // CoinGecko id
  char  symbol[12];  // text shown on screen
  float price;
  float change;      // 24 h % change
  bool  valid;
};

Coin coins[MAX_COINS];
int  numCoins = 0;

volatile bool refreshNow  = false;   // settings changed: fetch prices straight away
uint32_t      bannerUntil = 0;       // show the settings address in the footer until then

// Fear & Greed Index (alternative.me), shown as an extra item in the scroll
struct FearGreed {
  int  value;       // 0-100
  char label[20];   // "Extreme Fear" ... "Extreme Greed"
  bool valid;
};
FearGreed fng = {0, "", false};

// Colours (RGB565)
#define COL_BG      TFT_BLACK
#define COL_SYMBOL  0xFD20   // amber, exchange-board style
#define COL_PRICE   TFT_WHITE
#define COL_UP      0x07E0   // green
#define COL_DOWN    0xF800   // red
#define COL_DIM     0x7BEF   // grey
#define COL_LINE    0x2104   // dark grey

TFT_eSPI      tft;
TFT_eSprite   spr(&tft);

SemaphoreHandle_t dataMutex;
volatile uint32_t lastFetchMs = 0;
volatile bool     everFetched = false;

const int W = EXAMPLE_LCD_H_RES;   // 536
const int H = EXAMPLE_LCD_V_RES;   // 240
float scrollX = 0;

// Pause state
bool     paused       = false;
uint32_t pausedAtMs   = 0;

// Burn-in: pixel shift offsets (small orbit, max +/-4 px)
const int8_t SHIFT_PATTERN[][2] = {
  {0,0},{2,1},{4,2},{2,3},{0,4},{-2,3},{-4,2},{-2,1},
  {0,0},{-2,-1},{-4,-2},{-2,-3},{0,-4},{2,-3},{4,-2},{2,-1}
};
const int NUM_SHIFTS = sizeof(SHIFT_PATTERN) / sizeof(SHIFT_PATTERN[0]);
int ox = 0, oy = 0;

uint8_t currentBrightness = 0;

// ---------- helpers ----------
String fmtPrice(float p) {
  int dec = (p >= 1000) ? 0 : (p >= 1) ? 2 : 4;
  char buf[24];
  snprintf(buf, sizeof(buf), "%.*f", dec, p);
  String s(buf);
  int dot = s.indexOf('.');
  if (dot < 0) dot = s.length();
  for (int i = dot - 3; i > 0; i -= 3) s = s.substring(0, i) + "," + s.substring(i);
  return String(CURRENCY_SYM) + s;
}

String fmtChange(float c) {
  char buf[12];
  snprintf(buf, sizeof(buf), "%.2f%%", fabsf(c));
  return String(buf);
}

bool localTimeValid(struct tm& t) {
  return getLocalTime(&t, 0) && t.tm_year > (2020 - 1900);
}

// ---------- networking (runs on core 0) ----------
bool fetchPrices() {
  String ids;
  char cur[8];
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  for (int i = 0; i < numCoins; i++) {
    if (i) ids += ",";
    ids += coins[i].id;
  }
  strlcpy(cur, CURRENCY, sizeof(cur));
  xSemaphoreGive(dataMutex);

  if (ids.length() == 0) {                 // no coins configured
    lastFetchMs = millis();
    everFetched = true;
    return true;
  }

  String url = String("https://api.coingecko.com/api/v3/simple/price?ids=") + ids +
               "&vs_currencies=" + cur + "&include_24hr_change=true";

  WiFiClientSecure client;
  client.setInsecure();              // skips certificate check (public data only)
  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(client, url)) return false;

  int code = http.GET();
  if (code != 200) {
    Serial.printf("HTTP error %d\n", code);
    http.end();
    return false;
  }
  String payload = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    Serial.println("JSON parse failed");
    return false;
  }

  String chKey = String(cur) + "_24h_change";
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  if (strcmp(cur, CURRENCY) == 0) {        // ignore if currency changed mid-fetch
    for (int i = 0; i < numCoins; i++) {
      JsonObject o = doc[(const char*)coins[i].id];
      if (!o.isNull() && !o[cur].isNull()) {
        coins[i].price  = o[cur]   | 0.0f;
        coins[i].change = o[chKey] | 0.0f;
        coins[i].valid  = true;
      }
    }
    lastFetchMs = millis();
    everFetched = true;
  }
  xSemaphoreGive(dataMutex);
  Serial.println("Prices updated");
  return true;
}

bool fetchFearGreed() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(client, "https://api.alternative.me/fng/?limit=1")) return false;

  int code = http.GET();
  if (code != 200) {
    Serial.printf("F&G HTTP error %d\n", code);
    http.end();
    return false;
  }
  String payload = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, payload)) return false;
  JsonObject d = doc["data"][0];
  if (d.isNull()) return false;
  int v = atoi(d["value"] | "-1");
  if (v < 0 || v > 100) return false;

  xSemaphoreTake(dataMutex, portMAX_DELAY);
  fng.value = v;
  strlcpy(fng.label, d["value_classification"] | "", sizeof(fng.label));
  fng.valid = true;
  xSemaphoreGive(dataMutex);
  Serial.printf("Fear & Greed: %d (%s)\n", v, fng.label);
  return true;
}

void netTask(void*) {
  uint32_t nextPrice = millis(), nextFng = millis();
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.reconnect();
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }
    uint32_t now = millis();
    if (refreshNow) {                      // settings just changed
      refreshNow = false;
      nextPrice = now;
      if (SHOW_FEAR_GREED && !fng.valid) nextFng = now;
    }
    if ((int32_t)(now - nextPrice) >= 0) {
      nextPrice = millis() + (fetchPrices() ? FETCH_INTERVAL_MS : 15000);  // retry sooner on failure
    }
    if (SHOW_FEAR_GREED && (int32_t)(now - nextFng) >= 0) {
      nextFng = millis() + (fetchFearGreed() ? FNG_INTERVAL_MS : 60000);
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

// ---------- Wi-Fi setup (phone provisioning) ----------
WiFiManager wm;
String   apName;                 // e.g. "CryptoTicker-A1B2"
bool     holding     = false;    // button currently held down
uint32_t holdStartMs = 0;

void pushFrame() {
  lcd_PushColors(0, 0, W, H, (uint16_t*)spr.getPointer());
}

void drawMessage(const char* title, const char* line) {
  spr.fillSprite(COL_BG);
  spr.setTextDatum(MC_DATUM);
  spr.setFreeFont(&FreeSansBold18pt7b);
  spr.setTextColor(COL_SYMBOL);
  spr.drawString(title, W / 2, H / 2 - 22);
  spr.setFreeFont(&FreeSans12pt7b);
  spr.setTextColor(COL_DIM);
  spr.drawString(line, W / 2, H / 2 + 26);
  pushFrame();
}

String makeApName() {
  uint64_t mac = ESP.getEfuseMac();
  char buf[24];
  snprintf(buf, sizeof(buf), "CryptoTicker-%02X%02X",
           (uint8_t)(mac >> 32), (uint8_t)(mac >> 40));
  return String(buf);
}

// Shown while the setup network is open: QR code + 3 steps
void drawSetupScreen() {
  spr.fillSprite(COL_BG);

  // QR code that joins the open setup network when scanned with a phone camera
  String payload = "WIFI:T:nopass;S:" + apName + ";;";
  QRCode qr;
  uint8_t qrData[qrcode_getBufferSize(3)];
  qrcode_initText(&qr, qrData, 3, 0 /* ECC_LOW */, payload.c_str());
  const int scale = 6, quiet = 2;
  int box = (qr.size + quiet * 2) * scale;           // 198 px
  int qx = 16, qy = (H - box) / 2;
  spr.fillRect(qx, qy, box, box, TFT_WHITE);
  for (uint8_t y = 0; y < qr.size; y++)
    for (uint8_t x = 0; x < qr.size; x++)
      if (qrcode_getModule(&qr, x, y))
        spr.fillRect(qx + (x + quiet) * scale, qy + (y + quiet) * scale, scale, scale, TFT_BLACK);

  int tx = qx + box + 20;
  spr.setTextDatum(TL_DATUM);
  spr.setFreeFont(&FreeSansBold18pt7b);
  spr.setTextColor(COL_SYMBOL);
  spr.drawString("Wi-Fi Setup", tx, 22);

  spr.setFreeFont(&FreeSans9pt7b);
  spr.setTextColor(COL_PRICE);
  spr.drawString("1. Scan the code, or join:", tx, 76);
  spr.setFreeFont(&FreeSansBold12pt7b);
  spr.setTextColor(COL_UP);
  spr.drawString(apName, tx + 16, 100);
  spr.setFreeFont(&FreeSans9pt7b);
  spr.setTextColor(COL_PRICE);
  spr.drawString("2. Choose your home Wi-Fi", tx, 140);
  spr.drawString("3. Enter password, tap Save", tx, 166);
  spr.setTextColor(COL_DIM);
  spr.drawString("No page? Open 192.168.4.1", tx, 206);
  pushFrame();
}

void wifiReset() {
  Serial.println("Wi-Fi reset requested");
  drawMessage("Resetting Wi-Fi...", "Restarting into setup mode");
  wm.resetSettings();                 // erases the saved network
  delay(1500);
  ESP.restart();
}

void drawHoldOverlay(uint32_t now) {
  uint32_t held = now - holdStartMs;
  if (held > WIFI_RESET_HOLD_MS) held = WIFI_RESET_HOLD_MS;
  int left = (int)((WIFI_RESET_HOLD_MS - held + 999) / 1000);
  int bw = 380, bh = 96, bx = (W - bw) / 2, by = (H - bh) / 2;
  spr.fillRoundRect(bx, by, bw, bh, 12, COL_BG);
  spr.drawRoundRect(bx, by, bw, bh, 12, COL_SYMBOL);
  spr.setTextDatum(MC_DATUM);
  spr.setFreeFont(&FreeSansBold12pt7b);
  spr.setTextColor(COL_SYMBOL);
  spr.drawString("Keep holding to reset Wi-Fi", W / 2, by + 30);
  spr.setFreeFont(&FreeSansBold18pt7b);
  spr.setTextColor(COL_PRICE);
  spr.drawString(String(left), W / 2, by + 66);
}

// ---------- button ----------
// Short press (<1 s): pause / resume.  Hold 5 s: re-run Wi-Fi setup.
void handleButton(uint32_t now) {
  static bool     lastStable  = HIGH;
  static bool     lastReading = HIGH;
  static uint32_t lastChange  = 0;
  static bool     longDone    = false;

  bool reading = digitalRead(BUTTON_PIN);
  if (reading != lastReading) { lastChange = now; lastReading = reading; }

  if (now - lastChange > 40 && reading != lastStable) {   // 40 ms debounce
    lastStable = reading;
    if (lastStable == LOW) {                              // pressed
      holding = true;
      holdStartMs = now;
      longDone = false;
    } else {                                              // released
      holding = false;
      if (!longDone && now - holdStartMs < 1000) {
        paused = !paused;
        if (paused) pausedAtMs = now;
        Serial.println(paused ? "Paused" : "Resumed");
      }
    }
  }

  if (holding && !longDone && now - holdStartMs >= WIFI_RESET_HOLD_MS) {
    longDone = true;
    holding = false;
    wifiReset();
  }

  // Burn-in safety: don't leave a frozen image on screen forever
  if (paused && now - pausedAtMs > PAUSE_TIMEOUT_MS) {
    paused = false;
    Serial.println("Auto-resumed (pause timeout)");
  }
}

// ---------- burn-in protection ----------
void updateBurnInProtection(uint32_t now) {
  // Pixel shift
  int idx = (now / PIXEL_SHIFT_MS) % NUM_SHIFTS;
  ox = SHIFT_PATTERN[idx][0];
  oy = SHIFT_PATTERN[idx][1];

  // Night dimming (checked once a second)
  static uint32_t lastCheck = 0;
  if (now - lastCheck < 1000 && currentBrightness != 0) return;
  lastCheck = now;

  uint8_t target = BRIGHTNESS_DAY;
  struct tm t;
  if (localTimeValid(t)) {
    bool night = (NIGHT_START_HOUR > NIGHT_END_HOUR)
                   ? (t.tm_hour >= NIGHT_START_HOUR || t.tm_hour < NIGHT_END_HOUR)
                   : (t.tm_hour >= NIGHT_START_HOUR && t.tm_hour < NIGHT_END_HOUR);
    if (night) target = BRIGHTNESS_NIGHT;
  }
  if (target != currentBrightness) {
    lcd_setBrightness(target);
    currentBrightness = target;
  }
}

// ---------- drawing ----------
const int GAP_SMALL = 21;
const int GAP_ITEM  = 72;
const int ARROW_W   = 30;

int itemWidth(const Coin& c) {
  spr.setFreeFont(&FreeSansBold36pt7b);
  int w = spr.textWidth(c.symbol) + GAP_SMALL;
  if (c.valid) {
    w += spr.textWidth(fmtPrice(c.price)) + GAP_SMALL;
    w += ARROW_W + 9 + spr.textWidth(fmtChange(c.change));
  } else {
    w += spr.textWidth("---");
  }
  return w + GAP_ITEM;
}

int drawItem(const Coin& c, int x, int y) {
  spr.setFreeFont(&FreeSansBold36pt7b);
  spr.setTextDatum(ML_DATUM);
  int startX = x;

  spr.setTextColor(COL_SYMBOL);
  spr.drawString(c.symbol, x, y);
  x += spr.textWidth(c.symbol) + GAP_SMALL;

  if (!c.valid) {
    spr.setTextColor(COL_DIM);
    spr.drawString("---", x, y);
    x += spr.textWidth("---");
  } else {
    String p = fmtPrice(c.price);
    spr.setTextColor(COL_PRICE);
    spr.drawString(p, x, y);
    x += spr.textWidth(p) + GAP_SMALL;

    bool up = c.change >= 0;
    uint16_t col = up ? COL_UP : COL_DOWN;
    if (up) spr.fillTriangle(x, y + 12, x + ARROW_W, y + 12, x + ARROW_W / 2, y - 15, col);
    else    spr.fillTriangle(x, y - 12, x + ARROW_W, y - 12, x + ARROW_W / 2, y + 15, col);
    x += ARROW_W + 9;

    String ch = fmtChange(c.change);
    spr.setTextColor(col);
    spr.drawString(ch, x, y);
    x += spr.textWidth(ch);
  }

  // separator dot
  spr.fillCircle(x + GAP_ITEM / 2, y, 5, COL_LINE);
  return (x + GAP_ITEM) - startX;
}

uint16_t fngColor(int v) {
  if (v < 25) return COL_DOWN;   // Extreme Fear - red
  if (v < 45) return 0xFD20;     // Fear - orange
  if (v <= 55) return 0xFFE0;    // Neutral - yellow
  if (v <= 75) return 0x9FE0;    // Greed - light green
  return COL_UP;                 // Extreme Greed - green
}

const char* FNG_TITLE = "FEAR & GREED";

int fngWidth(const FearGreed& f) {
  spr.setFreeFont(&FreeSansBold36pt7b);
  int w = spr.textWidth(FNG_TITLE) + GAP_SMALL;
  if (f.valid) w += spr.textWidth(String(f.value)) + GAP_SMALL + spr.textWidth(f.label);
  else         w += spr.textWidth("---");
  return w + GAP_ITEM;
}

int drawFng(const FearGreed& f, int x, int y) {
  spr.setFreeFont(&FreeSansBold36pt7b);
  spr.setTextDatum(ML_DATUM);
  int startX = x;

  spr.setTextColor(COL_SYMBOL);
  spr.drawString(FNG_TITLE, x, y);
  x += spr.textWidth(FNG_TITLE) + GAP_SMALL;

  if (!f.valid) {
    spr.setTextColor(COL_DIM);
    spr.drawString("---", x, y);
    x += spr.textWidth("---");
  } else {
    uint16_t col = fngColor(f.value);
    String v = String(f.value);
    spr.setTextColor(col);
    spr.drawString(v, x, y);
    x += spr.textWidth(v) + GAP_SMALL;
    spr.drawString(f.label, x, y);
    x += spr.textWidth(f.label);
  }

  spr.fillCircle(x + GAP_ITEM / 2, y, 5, COL_LINE);
  return (x + GAP_ITEM) - startX;
}

void drawHeader(uint32_t now) {
  spr.setFreeFont(&FreeSansBold9pt7b);
  spr.setTextDatum(ML_DATUM);
  spr.setTextColor(COL_SYMBOL);

  String title = "CRYPTO";
  struct tm t;
  if (localTimeValid(t)) {
    char clk[8];
    strftime(clk, sizeof(clk), "%H:%M", &t);
    title += "   ";
    title += clk;
  }
  spr.drawString(title, 14 + ox, 22 + oy);

  String status;
  uint16_t col;
  if (paused) {
    uint32_t left = (PAUSE_TIMEOUT_MS - (now - pausedAtMs)) / 1000;
    status = "PAUSED  (" + String(left) + "s)";
    col = COL_SYMBOL;
  }
  else if (WiFi.status() != WL_CONNECTED) { status = "NO WIFI";       col = COL_DOWN; }
  else if (!everFetched)                  { status = "CONNECTING..."; col = COL_SYMBOL; }
  else {
    uint32_t age = (now - lastFetchMs) / 1000;
    if (age > (FETCH_INTERVAL_MS / 1000) * 3) { status = "STALE " + String(age) + "s";       col = COL_DOWN; }
    else                                      { status = "LIVE  " + String(age) + "s ago";   col = COL_UP; }
  }
  spr.setTextDatum(MR_DATUM);
  spr.setTextColor(col);
  spr.drawString(status, W - 14 + ox, 22 + oy);

  spr.drawFastHLine(0, 44 + oy, W, COL_LINE);
  spr.drawFastHLine(0, H - 44 + oy, W, COL_LINE);
}

void drawFooter() {
  spr.setFreeFont(&FreeSans9pt7b);
  spr.setTextDatum(MC_DATUM);
  spr.setTextColor(COL_DIM);
  String cur = String(CURRENCY);
  cur.toUpperCase();
  if (WiFi.status() != WL_CONNECTED) {
    spr.setTextColor(COL_SYMBOL);
    spr.drawString("No Wi-Fi  -  hold button 5 s to set up Wi-Fi", W / 2 + ox, H - 22 + oy);
    return;
  }
  if (paused || millis() < bannerUntil) {
    spr.setTextColor(COL_SYMBOL);
    spr.drawString("Settings:  " MDNS_NAME ".local   or   " + WiFi.localIP().toString(), W / 2 + ox, H - 22 + oy);
    return;
  }
  spr.drawString("24h change  |  prices in " + cur + "  |  CoinGecko", W / 2 + ox, H - 22 + oy);
}

// ---------- settings storage + web settings page ----------
Preferences prefs;
WebServer   server(80);

extern const char SETTINGS_HTML[];

const char* ALLOWED_CURRENCIES[] = {"aud", "usd", "nzd", "eur", "gbp", "cad", "sgd"};

bool currencyAllowed(const char* c) {
  for (const char* a : ALLOWED_CURRENCIES) if (strcmp(a, c) == 0) return true;
  return false;
}

// The large font only has plain ASCII, so non-dollar currencies use a code prefix
const char* currencySymbolFor(const char* c) {
  if (strcmp(c, "eur") == 0) return "EUR ";
  if (strcmp(c, "gbp") == 0) return "GBP ";
  return "$";
}

bool validCoinId(const char* s) {
  size_t n = strlen(s);
  if (n == 0 || n >= sizeof(Coin::id)) return false;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (!(islower(c) || isdigit(c) || c == '-' || c == '_' || c == '.')) return false;
  }
  return true;
}

// Caller must hold dataMutex (or be running before other tasks start)
void setDefaults() {
  strlcpy(CURRENCY, "aud", sizeof(CURRENCY));
  strlcpy(CURRENCY_SYM, "$", sizeof(CURRENCY_SYM));
  strlcpy(TZ_INFO, "AEST-10AEDT,M10.1.0,M4.1.0/3", sizeof(TZ_INFO));
  SHOW_FEAR_GREED  = true;
  SCROLL_SPEED_PPS = 110;
  BRIGHTNESS_DAY   = 180;
  BRIGHTNESS_NIGHT = 35;
  NIGHT_START_HOUR = 23;
  NIGHT_END_HOUR   = 7;
  numCoins = NUM_DEFAULT_COINS;
  for (int i = 0; i < numCoins; i++) {
    strlcpy(coins[i].id, DEFAULT_COINS[i].id, sizeof(coins[i].id));
    strlcpy(coins[i].symbol, DEFAULT_COINS[i].symbol, sizeof(coins[i].symbol));
    coins[i].price = coins[i].change = 0;
    coins[i].valid = false;
  }
}

void settingsToJson(JsonDocument& doc) {
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  doc["version"]     = FW_VERSION;
  doc["currency"]    = CURRENCY;
  doc["speed"]       = (int)SCROLL_SPEED_PPS;
  doc["dayBright"]   = BRIGHTNESS_DAY;
  doc["nightBright"] = BRIGHTNESS_NIGHT;
  doc["nightStart"]  = NIGHT_START_HOUR;
  doc["nightEnd"]    = NIGHT_END_HOUR;
  doc["tz"]          = TZ_INFO;
  doc["fng"]         = SHOW_FEAR_GREED;
  JsonArray arr = doc["coins"].to<JsonArray>();
  for (int i = 0; i < numCoins; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["id"]  = coins[i].id;
    o["sym"] = coins[i].symbol;
  }
  xSemaphoreGive(dataMutex);
}

void saveSettings() {
  JsonDocument doc;
  settingsToJson(doc);
  String out;
  serializeJson(doc, out);
  prefs.putString("cfg", out);
}

// Validates everything first, then applies. Returns "" on success or an error message.
String applySettingsJson(JsonDocument& doc, bool save) {
  const char* cur = doc["currency"] | CURRENCY;
  if (!currencyAllowed(cur)) return "Unsupported currency";

  const char* tz = doc["tz"] | TZ_INFO;
  if (strlen(tz) == 0 || strlen(tz) >= sizeof(TZ_INFO)) return "Invalid time zone";

  int speed  = constrain((int)(doc["speed"] | (int)SCROLL_SPEED_PPS), 20, 400);
  int dayB   = constrain((int)(doc["dayBright"] | (int)BRIGHTNESS_DAY), 5, 255);
  int nightB = constrain((int)(doc["nightBright"] | (int)BRIGHTNESS_NIGHT), 0, 255);
  int ns     = constrain((int)(doc["nightStart"] | NIGHT_START_HOUR), 0, 23);
  int ne     = constrain((int)(doc["nightEnd"] | NIGHT_END_HOUR), 0, 23);
  bool showF = doc["fng"] | SHOW_FEAR_GREED;

  JsonArray arr = doc["coins"];
  if (arr.isNull()) return "Missing coin list";
  if (arr.size() > MAX_COINS) return "Maximum of 20 coins";

  static Coin fresh[MAX_COINS];
  int n = 0;
  for (JsonObject o : arr) {
    const char* id  = o["id"]  | "";
    const char* sym = o["sym"] | "";
    if (!validCoinId(id)) return String("Invalid coin ID: ") + id;
    bool dup = false;
    for (int j = 0; j < n; j++) if (strcmp(fresh[j].id, id) == 0) dup = true;
    if (dup) continue;
    strlcpy(fresh[n].id, id, sizeof(fresh[n].id));
    // Symbol: uppercase printable ASCII, max 10 chars; fall back to the ID
    char s[11] = {0};
    int k = 0;
    const char* src = strlen(sym) ? sym : id;
    for (int i = 0; src[i] && k < 10; i++) {
      char c = src[i];
      if (c >= 32 && c < 127) s[k++] = toupper(c);
    }
    strlcpy(fresh[n].symbol, s, sizeof(fresh[n].symbol));
    fresh[n].price = fresh[n].change = 0;
    fresh[n].valid = false;
    n++;
  }

  xSemaphoreTake(dataMutex, portMAX_DELAY);
  bool curChanged = strcmp(cur, CURRENCY) != 0;
  if (!curChanged) {                       // keep prices for coins still in the list
    for (int i = 0; i < n; i++)
      for (int j = 0; j < numCoins; j++)
        if (strcmp(fresh[i].id, coins[j].id) == 0) {
          fresh[i].price  = coins[j].price;
          fresh[i].change = coins[j].change;
          fresh[i].valid  = coins[j].valid;
        }
  }
  memcpy(coins, fresh, sizeof(Coin) * n);
  numCoins = n;
  strlcpy(CURRENCY, cur, sizeof(CURRENCY));
  strlcpy(CURRENCY_SYM, currencySymbolFor(cur), sizeof(CURRENCY_SYM));
  bool tzChanged = strcmp(tz, TZ_INFO) != 0;
  strlcpy(TZ_INFO, tz, sizeof(TZ_INFO));
  SCROLL_SPEED_PPS = speed;
  BRIGHTNESS_DAY   = dayB;
  BRIGHTNESS_NIGHT = nightB;
  NIGHT_START_HOUR = ns;
  NIGHT_END_HOUR   = ne;
  SHOW_FEAR_GREED  = showF;
  xSemaphoreGive(dataMutex);

  if (tzChanged) { setenv("TZ", TZ_INFO, 1); tzset(); }
  refreshNow = true;
  if (save) saveSettings();
  return "";
}

void loadSettings() {
  prefs.begin("ticker", false);
  setDefaults();
  if (!prefs.isKey("cfg")) return;
  String s = prefs.getString("cfg", "");
  JsonDocument doc;
  if (deserializeJson(doc, s)) { Serial.println("Saved settings unreadable - using defaults"); return; }
  String err = applySettingsJson(doc, false);
  if (err.length()) {
    Serial.println("Saved settings invalid (" + err + ") - using defaults");
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    setDefaults();
    xSemaphoreGive(dataMutex);
  }
}

void sendJson(int code, JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  server.send(code, "application/json", out);
}

void handleGetSettings() {
  JsonDocument doc;
  settingsToJson(doc);
  sendJson(200, doc);
}

void handlePostSettings() {
  JsonDocument reply;
  JsonDocument doc;
  if (!server.hasArg("plain") || deserializeJson(doc, server.arg("plain"))) {
    reply["ok"] = false; reply["error"] = "Could not read settings";
    sendJson(400, reply);
    return;
  }
  String err = applySettingsJson(doc, true);
  reply["ok"] = err.length() == 0;
  if (err.length()) reply["error"] = err;
  sendJson(err.length() ? 400 : 200, reply);
  Serial.println(err.length() ? "Settings rejected: " + err : String("Settings saved"));
}

void handleReset() {
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  setDefaults();
  xSemaphoreGive(dataMutex);
  prefs.remove("cfg");
  setenv("TZ", TZ_INFO, 1); tzset();
  refreshNow = true;
  JsonDocument reply;
  reply["ok"] = true;
  sendJson(200, reply);
  Serial.println("Settings reset to defaults");
}

void startWebServer() {
  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html; charset=utf-8", SETTINGS_HTML);
  });
  server.on("/api/settings", HTTP_GET, handleGetSettings);
  server.on("/api/settings", HTTP_POST, handlePostSettings);
  server.on("/api/reset", HTTP_POST, handleReset);
  server.onNotFound([]() {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
  });
  server.begin();
}

// ---------- setup / loop ----------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("Crypto Ticker v" FW_VERSION);

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // IO38: LED on non-touch boards, screen power enable on touch boards
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);

  dataMutex = xSemaphoreCreateMutex();
  loadSettings();

  rm67162_init();
  lcd_setRotation(1);            // landscape 536x240
  lcd_setBrightness(BRIGHTNESS_DAY);
  currentBrightness = BRIGHTNESS_DAY;

  spr.createSprite(W, H);
  spr.setSwapBytes(1);   // if colours look wrong (e.g. blue/red swapped) set to 0
  if (!spr.created()) {
    while (true) { Serial.println("Sprite alloc failed - enable OPI PSRAM"); delay(1000); }
  }

  // ---- Wi-Fi: saved network, or phone setup portal ----
  apName = makeApName();
  drawMessage("Connecting to Wi-Fi...", "");
  WiFi.mode(WIFI_STA);

  wm.setTitle("Crypto Ticker");
  wm.setClass("invert");                      // dark theme
  std::vector<const char*> menu = {"wifi", "exit"};
  wm.setMenu(menu);                           // no firmware-update or erase pages
  wm.setShowInfoUpdate(false);
  wm.setConnectTimeout(20);
  wm.setConfigPortalTimeout(SETUP_TIMEOUT_S);
  wm.setAPCallback([](WiFiManager*) {
    Serial.println("Setup portal open: " + apName);
    drawSetupScreen();
  });

  if (!wm.autoConnect(apName.c_str())) {
    if (!wm.getWiFiIsSaved()) {
      // Never set up and nobody came: dim the screen and wait for a button press
      Serial.println("Setup timed out");
      lcd_setBrightness(25);
      drawMessage("Setup timed out", "Press the button to try again");
      while (digitalRead(BUTTON_PIN) == HIGH) delay(50);
      ESP.restart();
    }
    // Saved network unavailable right now: run the ticker and keep retrying
    Serial.println("Saved Wi-Fi not reachable - will keep retrying");
    WiFi.mode(WIFI_STA);
    WiFi.begin();
  }
  Serial.println("Wi-Fi: " + WiFi.SSID());

  // Clock via NTP (syncs automatically once Wi-Fi connects)
  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");

  // Settings page on the local network
  startWebServer();

  xTaskCreatePinnedToCore(netTask, "net", 12288, nullptr, 1, nullptr, 0);
  scrollX = W;  // start ticker off the right edge
}

void loop() {
  static uint32_t lastFrame    = millis();
  static bool     wasConnected = false;
  static bool     mdnsStarted  = false;
  uint32_t now = millis();
  float dt = (now - lastFrame) / 1000.0f;
  lastFrame = now;

  server.handleClient();

  // When Wi-Fi (re)connects: start mDNS and show the settings address for 30 s
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !wasConnected) {
    bannerUntil = now + 30000;
    if (!mdnsStarted && MDNS.begin(MDNS_NAME)) {
      MDNS.addService("http", "tcp", 80);
      mdnsStarted = true;
    }
    Serial.println("Settings page: http://" MDNS_NAME ".local  or  http://" + WiFi.localIP().toString());
  }
  wasConnected = connected;

  handleButton(now);
  updateBurnInProtection(now);

  // Snapshot data so the network task and settings page can update safely
  static Coin snap[MAX_COINS];
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  int n = numCoins;
  memcpy(snap, coins, sizeof(Coin) * n);
  FearGreed fngSnap = fng;
  bool  showF = SHOW_FEAR_GREED;
  float speed = SCROLL_SPEED_PPS;
  xSemaphoreGive(dataMutex);

  const int numItems = n + (showF ? 1 : 0);
  int cycleW = 0;
  for (int i = 0; i < n; i++) cycleW += itemWidth(snap[i]);
  if (showF) cycleW += fngWidth(fngSnap);

  spr.fillSprite(COL_BG);
  drawHeader(now);
  drawFooter();

  int y = H / 2 + oy;
  if (numItems == 0) {
    spr.setFreeFont(&FreeSansBold12pt7b);
    spr.setTextDatum(MC_DATUM);
    spr.setTextColor(COL_DIM);
    spr.drawString("No coins - add some on the settings page", W / 2 + ox, y);
  } else {
    // Scrolling ticker band (prices keep updating even while paused)
    int x = (int)scrollX;
    int i = 0;
    while (x < W) {
      int k = i % numItems;
      x += (k < n) ? drawItem(snap[k], x, y) : drawFng(fngSnap, x, y);
      i++;
    }
    if (!paused) {
      scrollX -= speed * dt;
      while (scrollX <= -cycleW) scrollX += cycleW;
    }
  }

  if (holding && now - holdStartMs > 1000) drawHoldOverlay(now);

  pushFrame();
}

// ---------- settings page (served at http://cryptoticker.local) ----------
const char SETTINGS_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Crypto Ticker Settings</title>
<style>
:root{--bg:#0b0b0c;--card:#17171a;--line:#2a2a2f;--txt:#eee;--dim:#8a8a93;--amber:#ffa31a;--green:#2ecc71;--red:#ff4d4d}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--txt);font:16px/1.4 -apple-system,system-ui,"Segoe UI",Roboto,sans-serif}
header{max-width:560px;margin:0 auto;padding:18px 12px 4px}
h1{margin:0;font-size:22px;color:var(--amber)}
.ver{color:var(--dim);font-size:13px}
main{max-width:560px;margin:0 auto;padding:0 12px 110px}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px;margin:12px 0}
h2{font-size:13px;margin:0 0 10px;color:var(--dim);text-transform:uppercase;letter-spacing:.06em}
.coin{display:flex;align-items:center;gap:6px;padding:8px 0;border-bottom:1px solid var(--line)}
.coin:last-child{border-bottom:0}
.sym{font-weight:700;color:var(--amber);min-width:64px}
.cid{flex:1;color:var(--dim);font-size:13px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
button{background:#26262b;color:var(--txt);border:1px solid var(--line);border-radius:8px;padding:9px 13px;font-size:15px;cursor:pointer}
button:disabled{opacity:.3}
button.icon{padding:6px 10px}
button.del{color:var(--red)}
button.primary{background:var(--amber);color:#111;border:0;font-weight:700;padding:11px 22px}
input,select{width:100%;background:#0f0f11;color:var(--txt);border:1px solid var(--line);border-radius:8px;padding:10px;font-size:16px}
input[type=range]{padding:0;accent-color:var(--amber)}
.row{display:flex;gap:8px;align-items:flex-end}
.row>*{flex:1}
label{display:block;margin:12px 0 6px;color:var(--dim);font-size:14px}
.res{display:flex;align-items:center;gap:10px;padding:8px;border-radius:8px;cursor:pointer}
.res:hover{background:#222}
.res img{width:24px;height:24px;border-radius:50%}
.res small{color:var(--dim)}
.toggle{display:flex;justify-content:space-between;align-items:center;margin-top:14px}
.toggle input{width:auto;transform:scale(1.4);accent-color:var(--amber)}
.bar{position:fixed;left:0;right:0;bottom:0;background:rgba(11,11,12,.92);border-top:1px solid var(--line);padding:12px}
.bar .in{max-width:560px;margin:0 auto;display:flex;gap:10px;align-items:center}
#msg{flex:1;font-size:14px;color:var(--dim)}
.hint{color:var(--dim);font-size:13px;margin:8px 0 0}
summary{cursor:pointer;color:var(--dim);font-size:14px;margin-top:12px}
</style></head><body>
<header><h1>Crypto Ticker</h1><div class="ver">Settings &middot; firmware v<span id="ver"></span></div></header>
<main>
<div class="card"><h2>Coins <span id="count"></span></h2><div id="coins"></div></div>

<div class="card"><h2>Add a coin</h2>
<div class="row"><input id="q" placeholder="Search, e.g. chainlink" autocomplete="off"><button id="go" style="flex:0 0 auto">Search</button></div>
<div id="results"></div>
<details><summary>Add by CoinGecko ID instead</summary>
<label>CoinGecko ID (the last part of the coin's CoinGecko page address)</label><input id="mid" placeholder="e.g. chainlink" autocapitalize="off">
<label>Symbol shown on the ticker</label><input id="msym" placeholder="e.g. LINK" maxlength="10">
<button id="madd" style="margin-top:10px">Add</button></details></div>

<div class="card"><h2>Display</h2>
<label>Currency</label><select id="currency"></select>
<label>Scroll speed: <b id="speedv"></b></label><input type="range" id="speed" min="30" max="300" step="5">
<div class="toggle"><span>Show Fear &amp; Greed Index</span><input type="checkbox" id="fng"></div></div>

<div class="card"><h2>Brightness &amp; night mode</h2>
<label>Day brightness: <b id="dayv"></b></label><input type="range" id="day" min="10" max="255">
<label>Night brightness: <b id="nightv"></b> <span class="hint">(0 = off)</span></label><input type="range" id="night" min="0" max="255">
<div class="row"><div><label>Night starts</label><select id="ns"></select></div><div><label>Night ends</label><select id="ne"></select></div></div>
<label>Time zone</label><select id="tz"></select></div>

<div class="card"><h2>Reset</h2><button id="reset">Restore default settings</button>
<p class="hint">To change Wi-Fi, hold the button on the ticker for 5 seconds.</p></div>
</main>
<div class="bar"><div class="in"><span id="msg"></span><button class="primary" id="save">Save</button></div></div>
<script>
const $=id=>document.getElementById(id);
const TZ=[["AEST-10AEDT,M10.1.0,M4.1.0/3","Sydney / Melbourne / Canberra / Hobart"],["AEST-10","Brisbane"],["ACST-9:30ACDT,M10.1.0,M4.1.0/3","Adelaide"],["ACST-9:30","Darwin"],["AWST-8","Perth"],["NZST-12NZDT,M9.5.0,M4.1.0/3","New Zealand"],["UTC0","UTC"]];
const CUR=[["aud","AUD - Australian dollar"],["usd","USD - US dollar"],["nzd","NZD - New Zealand dollar"],["eur","EUR - Euro"],["gbp","GBP - British pound"],["cad","CAD - Canadian dollar"],["sgd","SGD - Singapore dollar"]];
const MAX=20;
let S=null,dirty=false;
function msg(t,c){$('msg').textContent=t;$('msg').style.color=c||''}
function mark(){dirty=true;msg('Unsaved changes','var(--amber)')}
function hr(h){return (h%12||12)+(h<12?' am':' pm')}
function esc(s){const d=document.createElement('div');d.textContent=s==null?'':String(s);return d.innerHTML}
function opts(sel,list,val){sel.innerHTML='';list.forEach(([v,t])=>{const o=document.createElement('option');o.value=v;o.textContent=t;sel.appendChild(o)});
 if(val!==undefined&&!list.some(x=>String(x[0])===String(val))){const o=document.createElement('option');o.value=val;o.textContent='Custom: '+val;sel.appendChild(o)}
 sel.value=val}
function renderCoins(){const c=$('coins');c.innerHTML='';$('count').textContent='('+S.coins.length+'/'+MAX+')';
 if(!S.coins.length){c.innerHTML='<p class="hint">No coins yet. Add some below.</p>';return}
 S.coins.forEach((x,i)=>{const r=document.createElement('div');r.className='coin';
  r.innerHTML='<span class="sym">'+esc(x.sym)+'</span><span class="cid">'+esc(x.id)+'</span>';
  [['\u25B2',-1],['\u25BC',1]].forEach(([t,d])=>{const b=document.createElement('button');b.className='icon';b.textContent=t;b.title=d<0?'Move up':'Move down';
   b.disabled=(i+d<0||i+d>=S.coins.length);b.onclick=()=>{const j=i+d;[S.coins[i],S.coins[j]]=[S.coins[j],S.coins[i]];renderCoins();mark()};r.appendChild(b)});
  const del=document.createElement('button');del.className='icon del';del.textContent='\u2715';del.title='Remove';
  del.onclick=()=>{S.coins.splice(i,1);renderCoins();mark()};r.appendChild(del);c.appendChild(r)})}
function addCoin(id,sym){id=(id||'').trim().toLowerCase();sym=(sym||'').trim().toUpperCase().slice(0,10);
 if(!id)return;
 if(!/^[a-z0-9._-]+$/.test(id)){msg('That ID has invalid characters','var(--red)');return}
 if(S.coins.length>=MAX){msg('Maximum of '+MAX+' coins','var(--red)');return}
 if(S.coins.some(c=>c.id===id)){msg((sym||id)+' is already in the list','var(--amber)');return}
 S.coins.push({id:id,sym:sym||id.slice(0,6).toUpperCase()});renderCoins();mark()}
async function search(){const q=$('q').value.trim();if(!q)return;const r=$('results');r.innerHTML='<p class="hint">Searching...</p>';
 try{const res=await fetch('https://api.coingecko.com/api/v3/search?query='+encodeURIComponent(q));
  if(!res.ok)throw 0;const j=await res.json();const list=(j.coins||[]).slice(0,8);
  r.innerHTML=list.length?'':'<p class="hint">No matches.</p>';
  list.forEach(c=>{const d=document.createElement('div');d.className='res';
   d.innerHTML=(c.thumb?'<img src="'+esc(c.thumb)+'" alt="">':'')+'<div><b>'+esc(c.symbol)+'</b> '+esc(c.name)+'<br><small>'+esc(c.id)+(c.market_cap_rank?' &middot; rank #'+c.market_cap_rank:'')+'</small></div>';
   d.onclick=()=>{addCoin(c.id,c.symbol);r.innerHTML='';$('q').value=''};r.appendChild(d)})}
 catch(e){r.innerHTML='<p class="hint">Search is unavailable right now. Use "Add by CoinGecko ID" below.</p>'}}
function fill(){$('ver').textContent=S.version;renderCoins();
 opts($('currency'),CUR,S.currency);opts($('tz'),TZ,S.tz);
 const hours=[...Array(24).keys()].map(h=>[h,hr(h)]);opts($('ns'),hours,S.nightStart);opts($('ne'),hours,S.nightEnd);
 $('speed').value=S.speed;$('speedv').textContent=S.speed;
 $('day').value=S.dayBright;$('dayv').textContent=S.dayBright;
 $('night').value=S.nightBright;$('nightv').textContent=S.nightBright;
 $('fng').checked=S.fng;dirty=false;msg('')}
async function load(){try{const r=await fetch('/api/settings');S=await r.json();fill()}catch(e){msg('Could not load settings','var(--red)')}}
function collect(){S.currency=$('currency').value;S.tz=$('tz').value;S.nightStart=+$('ns').value;S.nightEnd=+$('ne').value;
 S.speed=+$('speed').value;S.dayBright=+$('day').value;S.nightBright=+$('night').value;S.fng=$('fng').checked}
$('save').onclick=async()=>{if(!S)return;collect();msg('Saving...');
 try{const r=await fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(S)});const j=await r.json();
  if(j.ok){dirty=false;msg('Saved. The ticker has been updated.','var(--green)')}else msg(j.error||'Save failed','var(--red)')}
 catch(e){msg('Save failed. Is the ticker switched on?','var(--red)')}};
$('reset').onclick=async()=>{if(!confirm('Restore all default settings?'))return;
 try{await fetch('/api/reset',{method:'POST'});await load();msg('Default settings restored.','var(--green)')}catch(e){msg('Reset failed','var(--red)')}};
$('go').onclick=search;$('q').onkeydown=e=>{if(e.key==='Enter')search()};
$('madd').onclick=()=>{addCoin($('mid').value,$('msym').value);$('mid').value='';$('msym').value=''};
['currency','tz','ns','ne','fng'].forEach(id=>$(id).onchange=mark);
[['speed','speedv'],['day','dayv'],['night','nightv']].forEach(([a,b])=>$(a).oninput=()=>{$(b).textContent=$(a).value;mark()});
window.onbeforeunload=e=>{if(dirty){e.preventDefault();e.returnValue=''}};
load();
</script></body></html>)rawliteral";
