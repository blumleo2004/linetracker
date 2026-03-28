# OTA Firmware Updates

Alle Wiener Linien Monitore pruefen automatisch alle 6 Stunden ob eine neue Firmware-Version verfuegbar ist. Wenn ja, wird das Update automatisch heruntergeladen und installiert.

## Neues Update veroeffentlichen

### 1. Version erhoehen

In `src/main.cpp` die Versionsnummer aendern:

```cpp
#define FW_VERSION "1.1.0"
```

### 2. Firmware kompilieren

```bash
cd "C:\dev\wiener linien monitor"
platformio run
```

Die fertige Binary liegt unter:
```
.pio/build/lilygo-t-display-s3/firmware.bin
```

### 3. GitHub Release erstellen

1. Gehe zu https://github.com/blumleo2004/linetracker/releases/new
2. Tag: `v1.1.0` (muss mit der Version uebereinstimmen)
3. Titel: `v1.1.0`
4. Beschreibung: Was sich geaendert hat
5. `firmware.bin` als Asset hochladen
6. Release veroeffentlichen

### 4. version.json aktualisieren

Die Datei `version.json` im `main` Branch muss auf die neue Version zeigen:

```json
{
    "version": "1.1.0",
    "url": "https://github.com/blumleo2004/linetracker/releases/download/v1.1.0/firmware.bin"
}
```

Sobald diese Datei gepusht ist, werden alle Monitore innerhalb von maximal 6 Stunden automatisch aktualisiert.

## Manuelles Update

Im Browser `linetracker.local/update` oeffnen um sofort nach einem Update zu suchen.

## Technische Details

- ESPs pruefen `https://raw.githubusercontent.com/blumleo2004/linetracker/main/version.json`
- Versionsvergleich ist semantisch (major.minor.patch)
- Waehrend dem Update zeigt das Display "Firmware Update..." und "Nicht ausschalten!"
- Nach erfolgreichem Update startet der Monitor automatisch neu
