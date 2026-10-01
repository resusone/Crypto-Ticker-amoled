# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html):

- **MAJOR** (2.0.0): changes that break existing setups, e.g. new hardware or a new required board package
- **MINOR** (1.1.0): new features, e.g. a new screen or new data source
- **PATCH** (1.0.1): bug fixes and small tweaks

## [Unreleased]

### Added
### Changed
### Fixed

## [1.2.0] - 2026-09-28

### Added
- Settings page at `http://cryptoticker.local` (or the ticker's IP address), usable from any phone or computer on the same Wi-Fi.
  - Add coins by searching CoinGecko, or by typing a CoinGecko ID. Remove and reorder coins. Up to 20 coins.
  - Change currency (AUD, USD, NZD, EUR, GBP, CAD, SGD), scroll speed, day and night brightness, night-mode hours, time zone, and Fear & Greed on/off.
  - Changes apply immediately and are saved on the ticker, so they survive restarts.
  - Restore default settings button.
- The settings address is shown in the footer for 30 seconds after connecting, and whenever the ticker is paused.
- A "No coins" message if the coin list is empty.

### Changed
- The coin list and display settings are no longer fixed in the code; the values in the sketch are now factory defaults.
- Prices refresh immediately after settings are saved.

## [1.1.0] - 2026-09-28

### Added
- Wi-Fi setup from a phone. On first start the ticker opens a `CryptoTicker-XXXX` setup network and shows a QR code; pick your Wi-Fi and enter the password in the page that opens. No code editing needed.
- Hold the BOOT button for 5 seconds to clear the saved Wi-Fi and re-run setup, with an on-screen countdown.
- The footer shows a Wi-Fi setup hint when the ticker is offline.
- The setup screen times out after 10 minutes, dims, and waits for a button press, to protect the screen from burn-in.
- A single ready-to-flash `.bin` is now attached to each release, so the ticker can be installed with a web flasher.

### Changed
- Wi-Fi details are no longer written in the code (`WIFI_SSID` / `WIFI_PASS` removed).
- Pause now triggers on button release, with a short press under 1 second, so it doesn't clash with the 5-second hold.
- New library required: WiFiManager (tzapu) 2.0.17.

## [1.0.0] - 2026-09-23

First public release.

### Added
- Smooth, flicker-free scrolling ticker for the LilyGO T-Display-S3 AMOLED 1.91" (536×240).
- Live prices for BTC, ETH, SOL, XRP, ADA and DOGE from the CoinGecko API (no API key needed), refreshed every 60 seconds.
- Prices in AUD, with the currency and symbol configurable.
- 24-hour change shown as a green ▲ or red ▼ percentage.
- Crypto Fear & Greed Index from alternative.me, coloured from red (Extreme Fear) to green (Extreme Greed), checked every 30 minutes.
- Header with a clock and connection status (LIVE, STALE, CONNECTING or NO WIFI).
- BOOT button to pause and resume the scroll.
- Burn-in protection:
  - Night dimming, 11 pm to 7 am by default.
  - Pixel shift of the static parts of the screen every 2 minutes.
  - Automatic resume after 5 minutes paused.
- Network fetching runs on a separate core, so the scroll never pauses during updates.
- Large 36pt bold font for the ticker text.
- Firmware version printed to the Serial Monitor at boot.
- README, CC BY-NC 4.0 licence, `.gitignore` and Ko-fi funding link.

[Unreleased]: https://github.com/resusone/Crypto-Ticker-amoled/compare/v1.2.0...HEAD
[1.2.0]: https://github.com/resusone/Crypto-Ticker-amoled/compare/v1.1.0...v1.2.0
[1.1.0]: https://github.com/resusone/Crypto-Ticker-amoled/compare/v1.0.0...v1.1.0
[1.0.0]: https://github.com/resusone/Crypto-Ticker-amoled/releases/tag/v1.0.0
