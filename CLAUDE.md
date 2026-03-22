# Zymoscope — Project Context for Claude

## What is this?

Zymoscope is an ESP32-based fermentation monitor and temperature controller
with a self-hosted web dashboard. It tracks temperature, estimates specific
gravity via load cell weight loss, logs ambient conditions, and controls
heat/cool relays via PID.

Inspired by [OpenAg FermentaBot](https://github.com/OpenAgricultureFoundation/fermentabot),
rebuilt from scratch.

## Architecture

- **ESP32 sensor node** — collects data, runs PID, publishes via MQTT
- **Web app** (FastAPI + SQLite + Chart.js) — subscribes to MQTT, stores
  telemetry, serves a dark-themed dashboard with live charts
- **MQTT is the contract** between them — the ESP32 and dashboard are
  fully decoupled

Future: Raspberry Pi + camera module for krausen/visual tracking. The Pi
runs the web app and adds a camera pipeline. The ESP32 node stays dumb.

## Repository structure

- `firmware/` — ESP-IDF v5.x C firmware
  - `main/sensor/` — ds18b20 (1-Wire), hx711 (load cell ADC), bme280 (I2C)
  - `main/display/` — SSD1306 OLED driver with 8x8 font
  - `main/control/` — PID temperature controller
  - `main/comms/` — Wi-Fi STA + MQTT client
  - `main/app_main.c` — wires everything, runs 3 FreeRTOS tasks
- `dashboard/` — Python web app
  - `zymoscope/` — FastAPI server, MQTT subscriber, SQLite DB, config, smart plug control
  - `zymoscope/smart_plug.py` — TP-Link Kasa plug control via python-kasa (KP115 energy monitoring)
  - `templates/index.html` — single-file dark-themed SPA (Chart.js, vanilla JS)
  - `docker-compose.yml` — optional Mosquitto + InfluxDB + Grafana stack
- `hardware/` — KiCad schematic (placeholder), BOM CSV
- `docs/` — design.md, prototype-bom.md, getting-started.md

## Build commands

```bash
# Firmware (requires ESP-IDF v5.x sourced)
cd firmware/
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor

# Web dashboard
cd dashboard/
pip install -r requirements.txt
python -m zymoscope.server
```

## Key conventions

- MQTT topics: `zymoscope/<mac>/telemetry`, `zymoscope/<mac>/cmd`, `zymoscope/<mac>/status`
- ESP32 log tag: `"zymoscope"`
- Default Wi-Fi SSID placeholder: `ZymoNet` (in `firmware/main/comms/wifi_sta.h`)
- Default MQTT broker URI: `mqtt://192.168.1.100:1883` (in `firmware/main/app_main.c`)
- Dashboard runs on port 8000 by default
- Python package is `zymoscope` (under `dashboard/zymoscope/`)
- Smart plug env vars: `KASA_HEATER_HOST`, `KASA_COOLER_HOST` (IP addresses, empty = disabled)
- python-kasa controls TP-Link Kasa plugs locally (no cloud). KP115 provides energy monitoring.

## Hardware (prototype)

ESP32-DevKitC + breakout modules on a breadboard. ~$45-55 total.

| Sensor/Module | GPIO | Interface |
|---------------|------|-----------|
| DS18B20 temp probe | GPIO 4 | 1-Wire (4.7k pull-up) |
| HX711 load cell ADC | GPIO 18 (SCK), 19 (DOUT) | Bit-bang |
| BME280 ambient | GPIO 21 (SDA), 22 (SCL) | I2C @ 0x76 |
| SSD1306 OLED | GPIO 21 (SDA), 22 (SCL) | I2C @ 0x3C |
| Relay 1 (heater) | GPIO 25 | Digital out |
| Relay 2 (cooler) | GPIO 26 | Digital out |

## Current status

- Firmware compiles and is ready to flash (ESP-IDF v5.2.2)
- Web dashboard code is complete (not yet tested against live MQTT)
- Smart plug integration complete (Kasa KP115 — heater/cooler control + energy monitoring)
- Parts have been ordered but not yet received
- KiCad schematic is a placeholder skeleton — real PCB design is future work

## Known issues fixed during development

- `ds18b20.c` needed FreeRTOS includes (`freertos/FreeRTOS.h`, `freertos/task.h`)
- `mqtt_client.c` had header collision — ESP-IDF's `mqtt_client.h` must be
  included with angle brackets (`<mqtt_client.h>`) not quotes, and needs
  `esp_event.h` for `esp_event_base_t`

## What's next (roadmap)

1. Wire prototype when parts arrive, validate sensors one at a time
2. Calibrate HX711 load cell with known weight
3. Test web dashboard against live MQTT data
4. Raspberry Pi integration + camera module (krausen tracking)
5. pH probe support
6. OTA firmware updates
7. Custom PCB (KiCad, JLCPCB-ready)
8. 3D-printable enclosure (OpenSCAD)

## Cleanup needed

Old files that should be deleted (rename from fermentabot -> zymoscope):
- `dashboard/fermentabot/` — replaced by `dashboard/zymoscope/`
- `hardware/fermentabot-v2.kicad_pro` — replaced by `hardware/zymoscope.kicad_pro`
- `hardware/fermentabot-v2.kicad_sch` — replaced by `hardware/zymoscope.kicad_sch`
- `hardware/bom/fermentabot-v2-bom.csv` — replaced by `hardware/bom/zymoscope-bom.csv`

## License

GPL-3.0 (software), CERN-OHL-S-2.0 (hardware)
