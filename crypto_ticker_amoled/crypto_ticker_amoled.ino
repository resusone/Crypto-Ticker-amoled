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

  Build requirements:
    - "esp32 by Espressif Systems" board package v2.0.17
    - TFT_eSPI (Bodmer, 2.5.43) - used only for the off-screen sprite
    - ArduinoJson (v7.x)
    - pins_config.h, rm67162.h, rm67162.cpp in the same folder

  Board settings:
    ESP32S3 Dev Module, USB CDC On Boot: Enabled, Flash Size: 16MB,
    PSRAM: OPI PSRAM, Partition: 16M Flash (3MB APP/9.9MB FATFS)
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include "rm67162.h"
#include "FreeSansBold36pt7b.h"   // 50% larger than the 24pt font

#ifndef BOARD_HAS_PSRAM
#error "Set PSRAM: OPI PSRAM in the Tools menu"
#endif
#include <time.h>

// ================== USER SETTINGS ==================
const char* WIFI_SSID     = "YOUR_WIFI_NAME";
const char* WIFI_PASS     = "YOUR_WIFI_PASSWORD";

const char* CURRENCY      = "aud";
const char* CURRENCY_SYM  = "$";      // change to "A$" if you prefer

// Time zone (POSIX format). Default = AEST/AEDT (Sydney, Melbourne, Canberra, Hobart).
//   Brisbane (no DST): "AEST-10"
//   Adelaide:          "ACST-9:30ACDT,M10.1.0,M4.1.0/3"
//   Perth:             "AWST-8"
const char* TZ_INFO       = "AEST-10AEDT,M10.1.0,M4.1.0/3";

const uint32_t FETCH_INTERVAL_MS = 60000; // 60 s keeps well inside free rate limits
const uint32_t FNG_INTERVAL_MS   = 1800000; // Fear & Greed updates daily; check every 30 min
const bool     SHOW_FEAR_GREED   = true;
const float    SCROLL_SPEED_PPS  = 110.0;  // scroll speed in pixels per second

// ---- Burn-in protection ----
const uint8_t  BRIGHTNESS_DAY    = 180;   // 0-255
const uint8_t  BRIGHTNESS_NIGHT  = 35;    // 0-255 (0 = screen effectively off)
const int      NIGHT_START_HOUR  = 23;    // dim from 11 pm...
const int      NIGHT_END_HOUR    = 7;     // ...until 7 am
const uint32_t PIXEL_SHIFT_MS    = 120000;// move static elements every 2 min
const uint32_t PAUSE_TIMEOUT_MS  = 300000;// auto-resume after 5 min paused

// ---- Button ----
const int      BUTTON_PIN        = PIN_BUTTON_1;  // BOOT button (GPIO0), active LOW

struct Coin {
  const char* id;      // CoinGecko id
  const char* symbol;  // text shown on screen
  float price;
  float change;        // 24 h % change
  bool  valid;
};

Coin coins[] = {
  {"bitcoin",  "BTC",  0, 0, false},
  {"ethereum", "ETH",  0, 0, false},
  {"solana",   "SOL",  0, 0, false},
  {"ripple",   "XRP",  0, 0, false},
  {"cardano",  "ADA",  0, 0, false},
  {"dogecoin", "DOGE", 0, 0, false},
};
// ===================================================

const int NUM_COINS = sizeof(coins) / sizeof(coins[0]);

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
  for (int i = 0; i < NUM_COINS; i++) {
    if (i) ids += ",";
    ids += coins[i].id;
  }
  String url = String("https://api.coingecko.com/api/v3/simple/price?ids=") + ids +
               "&vs_currencies=" + CURRENCY + "&include_24hr_change=true";

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

  String chKey = String(CURRENCY) + "_24h_change";
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  for (int i = 0; i < NUM_COINS; i++) {
    JsonObject o = doc[coins[i].id];
    if (!o.isNull()) {
      coins[i].price  = o[CURRENCY] | 0.0f;
      coins[i].change = o[chKey]    | 0.0f;
      coins[i].valid  = true;
    }
  }
  lastFetchMs = millis();
  everFetched = true;
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
  uint32_t nextPrice = 0, nextFng = 0;
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.reconnect();
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }
    uint32_t now = millis();
    if ((int32_t)(now - nextPrice) >= 0) {
      nextPrice = millis() + (fetchPrices() ? FETCH_INTERVAL_MS : 15000);  // retry sooner on failure
    }
    if (SHOW_FEAR_GREED && (int32_t)(now - nextFng) >= 0) {
      nextFng = millis() + (fetchFearGreed() ? FNG_INTERVAL_MS : 60000);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// ---------- button ----------
void handleButton(uint32_t now) {
  static bool     lastStable  = HIGH;
  static bool     lastReading = HIGH;
  static uint32_t lastChange  = 0;

  bool reading = digitalRead(BUTTON_PIN);
  if (reading != lastReading) { lastChange = now; lastReading = reading; }

  if (now - lastChange > 40 && reading != lastStable) {   // 40 ms debounce
    lastStable = reading;
    if (lastStable == LOW) {                              // pressed
      paused = !paused;
      if (paused) pausedAtMs = now;
      Serial.println(paused ? "Paused" : "Resumed");
    }
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
  spr.drawString("24h change  |  prices in " + cur + "  |  CoinGecko", W / 2 + ox, H - 22 + oy);
}

// ---------- setup / loop ----------
void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // IO38: LED on non-touch boards, screen power enable on touch boards
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);

  rm67162_init();
  lcd_setRotation(1);            // landscape 536x240
  lcd_setBrightness(BRIGHTNESS_DAY);
  currentBrightness = BRIGHTNESS_DAY;

  spr.createSprite(W, H);
  spr.setSwapBytes(1);   // if colours look wrong (e.g. blue/red swapped) set to 0
  if (!spr.created()) {
    while (true) { Serial.println("Sprite alloc failed - enable OPI PSRAM"); delay(1000); }
  }

  dataMutex = xSemaphoreCreateMutex();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  // Clock via NTP (syncs automatically once Wi-Fi connects)
  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");

  xTaskCreatePinnedToCore(netTask, "net", 12288, nullptr, 1, nullptr, 0);
  scrollX = W;  // start ticker off the right edge
}

void loop() {
  static uint32_t lastFrame = millis();
  uint32_t now = millis();
  float dt = (now - lastFrame) / 1000.0f;
  lastFrame = now;

  handleButton(now);
  updateBurnInProtection(now);

  // Snapshot coin data so the network task can update safely
  Coin snap[NUM_COINS];
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  memcpy(snap, coins, sizeof(coins));
  FearGreed fngSnap = fng;
  xSemaphoreGive(dataMutex);

  const int numItems = NUM_COINS + (SHOW_FEAR_GREED ? 1 : 0);

  int cycleW = 0;
  for (int i = 0; i < NUM_COINS; i++) cycleW += itemWidth(snap[i]);
  if (SHOW_FEAR_GREED) cycleW += fngWidth(fngSnap);

  spr.fillSprite(COL_BG);
  drawHeader(now);
  drawFooter();

  // Scrolling ticker band (prices keep updating even while paused)
  int y = H / 2 + oy;
  int x = (int)scrollX;
  int i = 0;
  while (x < W) {
    int k = i % numItems;
    x += (k < NUM_COINS) ? drawItem(snap[k], x, y) : drawFng(fngSnap, x, y);
    i++;
  }

  if (!paused) {
    scrollX -= SCROLL_SPEED_PPS * dt;
    while (scrollX <= -cycleW) scrollX += cycleW;
  }

  lcd_PushColors(0, 0, W, H, (uint16_t*)spr.getPointer());
}
