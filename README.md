# Wiener Linien Abfahrtsmonitor

Real-time Vienna public transport departure display for the LILYGO T-Display-S3.

Inspired by the dot-matrix amber LED passenger information displays found at tram and U-Bahn stops in Vienna.

![Display showing amber departure info on dark background](https://github.com/user-attachments/assets/placeholder)

## Features

- **Real-time departures** from Wiener Linien (U-Bahn, Tram, Bus) and OeBB (S-Bahn, REX, trains)
- **Dot-matrix amber LED style** display — looks like a real Fahrgastinformationssystem
- **Smart display logic** — groups departures by line+direction, rotates pages fairly so no line is starved
- **Scrolling direction text** — long destination names scroll automatically
- **Web config interface** — set up entirely via browser, no coding needed
- **Station search & browse** — search by name or browse A-Z
- **WiFi management** — change WiFi without losing your line configuration
- **OeBB train support** — add S-Bahn and train stations with line/direction selection
- **Night mode** — auto-dim at configurable hours
- **Adjustable settings** — page switch speed, brightness, night brightness

## Hardware

- **Board:** LILYGO T-Display-S3 1.9" (170x320px ST7789, 8-bit parallel interface)
- **Framework:** PlatformIO + Arduino + ESP-IDF (FreeRTOS)

## Setup

### First time

1. Flash the firmware (see below)
2. Connect to WiFi network **"WienerLinienMonitor"** from your phone/laptop
3. Select your home WiFi network and enter the password
4. The display will show the ESP's IP address
5. Open that IP in your browser

### Adding lines (Wiener Linien)

1. Open the web interface in your browser
2. Type a station name in the search box (e.g. "Kutschkergasse") or browse A-Z
3. Select the specific lines and directions you want
4. Click "Ausgewaehlte hinzufuegen"

### Adding trains (OeBB)

1. Open the web interface
2. Under "S-Bahn / Zuege" enter a station name (e.g. "Wien Rennweg")
3. The ESP will search for the canonical station name and load available lines
4. Select specific line+direction combinations (e.g. S 3 → Meidling)

### Settings

- **Seitenwechsel** — how long each page is shown before rotating (2-30s, default 5s)
- **Helligkeit** — display brightness (applies immediately)
- **Nachtmodus** — auto-dim between configurable hours (e.g. 22:00-07:00)

### Buttons

| Action | Result |
|--------|--------|
| Hold BOOT 3s, release | Open WiFi portal (line config is kept) |
| Hold BOOT 10s | Factory reset (erases all config) |

## Flashing

```bash
# Install PlatformIO, then:
cd "wiener linien monitor"
pio run --target upload
```

Upload speed is set to 115200 baud for reliability. If the upload fails, hold BOOT while pressing RST to enter download mode manually.

## APIs used

| Source | Endpoint |
|--------|----------|
| Wiener Linien real-time | `wienerlinien.at/ogd_realtime/monitor?rbl=...` |
| Wiener Linien stations (CSV) | `data.wien.gv.at/csv/wienerlinien-ogd-haltestellen.csv` |
| Wiener Linien RBLs (CSV) | `data.wien.gv.at/csv/wienerlinien-ogd-steige.csv` |
| OeBB departures | `fahrplan.oebb.at/bin/stboard.exe/dn?outputMode=tickerDataOnly` |
| OeBB station search | `fahrplan.oebb.at/bin/ajax-getstop.exe/dn` |

No API keys required for any of these.

## Configuration storage

Config is saved to SPIFFS at `/config.json`:

```json
{
  "lines": [
    { "rbl": "187", "name": "40", "towards": "Schottentor", "type": "ptTram" }
  ],
  "oebb": [
    { "station": "Wien Rennweg", "line": "S 3", "towards": "Meidling Hauptstrasse" }
  ],
  "rotate_sec": 5,
  "brightness": 255,
  "night_from": 22,
  "night_to": 7,
  "night_bright": 20
}
```

## Libraries

- [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) — display driver
- [ArduinoJson](https://arduinojson.org/) v7 — JSON parsing
- [WiFiManager](https://github.com/tzapu/WiFiManager) — WiFi setup portal

## Reference

Inspired by [coppermilk/wiener_linien_esp32_monitor](https://github.com/coppermilk/wiener_linien_esp32_monitor).
