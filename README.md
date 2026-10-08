# kegMiniDisplay

A tiny kegerator dashboard on a **Waveshare ESP32-C6-LCD-1.47** (1.47" 172×320 ST7789 screen).
It reads `http://kegerator.local/api` and shows keg pressure on a 0–30 PSI gauge, plus beer and air temperature.

Built as a learning project for driving a small screen from an ESP32.

## Features
- **WiFi setup portal** – if it can't join WiFi it starts an open access point `KegDisplay-Setup`; join it from a phone, pick your network, enter the password. Credentials are saved in flash.
- **Reset WiFi** – press BOOT during the 2-second splash screen.
- **Analog-style PSI gauge** – drawn off-screen on a canvas and flushed in one go (no flicker), with an eased needle and green/red zones.
- **Stale data** – values turn grey if the kegerator stops answering.
- **OTA updates** – upload from the Arduino IDE over WiFi.
- **Portrait or landscape** – set `ROTATION` at the top of the sketch.

## API
The data comes from the kegerator itself, running [kegerator-gauge](https://github.com/kd4gar/kegerator-gauge).
It returns JSON like:
```json
{"psi": 12.3, "kegF": 41.3, "airF": 41.3}
```
Negative PSI readings are shown as 0.

## Building (Arduino IDE)
**Libraries** (Library Manager):
- GFX Library for Arduino (Moon On Our Nation)
- ArduinoJson v7 (Benoit Blanchon)

**Board settings** (esp32 by Espressif, 3.x):
| Setting | Value |
|---|---|
| Board | ESP32C6 Dev Module |
| Partition Scheme | Minimal SPIFFS (1.9MB APP with OTA/128KB SPIFFS) |
| USB CDC On Boot | Enabled (only needed for Serial Monitor over USB) |

The first upload (or any partition scheme change) must go over USB. After that, choose the `kegdisplay at <ip>` network port and upload over WiFi.

## Configuration
Constants near the top of `KegDisplay/KegDisplay.ino`:

| Constant | Purpose |
|---|---|
| `KEG_HOST`, `KEG_PATH` | where the API lives (`kegerator` + `/api`) |
| `ROTATION` | 0/2 portrait, 1/3 landscape |
| `ZONE_GOOD_LOW`, `ZONE_GOOD_HIGH`, `ZONE_HIGH` | gauge colour bands (psi) |
| `OTA_HOSTNAME`, `OTA_PASSWORD` | OTA identity |

## Hardware pins
| Signal | GPIO |
|---|---|
| LCD MOSI / SCLK | 6 / 7 |
| LCD CS / DC / RST | 14 / 15 / 21 |
| Backlight | 22 |
| BOOT button | 9 |
