# LineTracker

**Real-time public transport departure display for Vienna.**

by Leo Blum

---

LineTracker turns a LILYGO T-Display-S3 into a compact departure monitor that looks like the amber dot-matrix passenger information displays found at Vienna's tram and U-Bahn stops. Set it up once via your phone, and it shows live countdowns for your lines — no coding required.

## Features

- **Real-time departures** — U-Bahn, Tram, Bus (Wiener Linien) and S-Bahn/REX (OeBB)
- **Dot-matrix amber LED style** — authentic Fahrgastinformationssystem look
- **Web-based setup** — configure everything from your browser at `linetracker.local`
- **Station search & browse** — find any stop by name or browse A-Z
- **Smart display** — groups by line + direction, rotates pages, scrolls long text
- **Night mode** — auto-dim between configurable hours
- **Over-the-air updates** — firmware updates automatically over WiFi
- **WiFi management** — change networks without losing your configuration
- **Gift-friendly** — designed to be set up by anyone, no technical knowledge needed

## Hardware

| Component | Specification |
|-----------|--------------|
| Board | LILYGO T-Display-S3 1.9" |
| Display | 170x320px ST7789, 8-bit parallel |
| Framework | PlatformIO + Arduino + ESP-IDF (FreeRTOS) |

## Quick Start

### 1. Flash

```bash
git clone https://github.com/blumleo2004/linetracker.git
cd linetracker
pio run --target upload
```

Upload speed is 115200 baud. If upload fails, hold BOOT while pressing RST to enter download mode.

### 2. Connect

1. On your phone/laptop, connect to the WiFi network **"LineTracker"**
2. Select your home WiFi and enter the password
3. Open **linetracker.local** in your browser

### 3. Add Lines

**Wiener Linien (U-Bahn, Tram, Bus):**
Search for a station name or browse A-Z, then select the lines and directions you want.

**OeBB (S-Bahn, REX):**
Under "S-Bahn / Zuege", search for a station (e.g. "Wien Rennweg") and select line + direction.

Lines that are not currently running (e.g. U-Bahn at night) are still shown and can be added.

## Settings

| Setting | Description |
|---------|-------------|
| Seitenwechsel | Page rotation interval (2-30 seconds) |
| Helligkeit | Display brightness |
| Nachtmodus | Auto-dim between configurable hours |
| Naechste Abfahrt | Show next departure below countdown |
| Stoerungsticker | Show disruption alerts at bottom |

## Buttons

| Action | Result |
|--------|--------|
| Hold BOOT 3s | WiFi reset (opens setup portal, keeps config) |
| Hold BOOT 10s | Factory reset (erases everything) |

## OTA Updates

LineTracker checks for firmware updates automatically every 6 hours. You can also trigger a manual check via **linetracker.local/update**.

See [OTA_UPDATE.md](OTA_UPDATE.md) for how to publish new releases.

## Data Sources

| Source | Provider | License |
|--------|----------|---------|
| Real-time departures | [Wiener Linien](https://www.wienerlinien.at) | OGD |
| Station & line data | [Stadt Wien – data.wien.gv.at](https://data.wien.gv.at) | CC BY 4.0 |
| S-Bahn / train departures | [OeBB/SCOTTY](https://fahrplan.oebb.at) | — |

No API keys required.

## Libraries

- [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) — display driver
- [ArduinoJson](https://arduinojson.org/) v7 — JSON parsing
- [WiFiManager](https://github.com/tzapu/WiFiManager) — WiFi setup portal

## License

MIT

---

*Inspired by [coppermilk/wiener_linien_esp32_monitor](https://github.com/coppermilk/wiener_linien_esp32_monitor)*
