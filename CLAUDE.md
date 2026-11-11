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
  - `main/comms/` — Wi-Fi STA + MQTT client + OTA (`ota.c`)
  - `partitions.csv` — 4 MB, two OTA slots (nvs kept at 0x9000/0x6000)
  - `tools/ota_push.py` — push a build over the air (see `docs/ota.md`)
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
idf.py -p /dev/ttyUSB0 flash monitor     # USB (needed once for OTA layout)
python tools/ota_push.py                 # over the air, after that

# Web dashboard
cd dashboard/
pip install -r requirements.txt
python -m zymoscope.server
```

## Key conventions

- MQTT topics: `zymoscope/<mac>/telemetry`, `zymoscope/<mac>/cmd`, `zymoscope/<mac>/status`
- `status` topic: retained `online` (fw, partition, ota_state) / `offline` LWT, plus `ota` progress events
- MQTT cmds: `{"setpoint": <float>}`, `{"ota_url": "<http url>"}` (never publish cmds retained)
- ESP-IDF lives at `~/esp/esp-idf` — `. ~/esp/esp-idf/export.sh` before `idf.py`
- ESP32 log tag: `"zymoscope"`
- Default MQTT broker URI: `mqtt://192.168.1.100:1883` (in `firmware/main/app_main.c`)
- PID setpoint persisted in NVS (`zymoscope` namespace, `setpoint` key)
- Dashboard runs on port 8000 by default
- Charts use `/api/series/<id>?hours=` (SQL time-bucketed, ≤~1500 pts; implausible temps dropped) — don't fetch raw `/api/telemetry` for long ranges (full history was 21 MB / 150 s on the Pi)
- `/api/export/<id>.csv?hours=` streams raw telemetry; `POST /api/devices/<id>` `{"name"}` renames; `index.html?range=<hours>` sets the default chart range
- Python package is `zymoscope` (under `dashboard/zymoscope/`)
- Smart plug env vars: `KASA_HEATER_HOST`, `KASA_COOLER_HOST` (IP addresses, empty = disabled)
- `PLUG_WATCHDOG_SEC` (default 180): dashboard switches plugs off if no telemetry arrives for this long; 0 disables
- Firmware fails safe: no valid DS18B20 reading → both relays OFF (no BME280 fallback)
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

- Firmware running on ESP32 with DS18B20, BME280, OLED working (ESP-IDF v5.2.2)
- Web dashboard tested against live MQTT — temperature chart, plug status, setpoint control
- Kasa KP115 smart plug controlling seedling heat mat via PID
- DS18B20 driver has critical sections (portENTER_CRITICAL) + retry logic for noise resilience
- PID setpoint persists in NVS across reboots
- HX711 load cell and relay module are optional (not wired in current prototype)
- KiCad schematic is a placeholder skeleton — real PCB design is future work

## Known issues fixed during development

- `ds18b20.c` needed FreeRTOS includes (`freertos/FreeRTOS.h`, `freertos/task.h`)
- `mqtt_client.c` had header collision — ESP-IDF's `mqtt_client.h` must be
  included with angle brackets (`<mqtt_client.h>`) not quotes, and needs
  `esp_event.h` for `esp_event_base_t`
- Unsoldered BME280/OLED header pins cause noise on the shared power rail,
  corrupting DS18B20 1-Wire reads (CRC mismatches). Fix: solder pins + critical
  sections in the 1-Wire driver.
- `db.py` had double-await bug with aiosqlite (`async with await _get_conn()`)
  — fixed with asynccontextmanager pattern
- `server.py` TemplateResponse API changed in newer Starlette — request is now
  the first positional arg
- Telemetry JSON key was `temps` but dashboard expected `temp_c` — renamed

## What's next (roadmap)

1. Raspberry Pi integration + camera module (krausen tracking)
2. pH probe support
3. ~~OTA firmware updates~~ (done — see docs/ota.md)
4. Custom PCB (KiCad, JLCPCB-ready)
5. 3D-printable enclosure (OpenSCAD)

## Cleanup needed

Old files that should be deleted (rename from fermentabot -> zymoscope):
- `dashboard/fermentabot/` — replaced by `dashboard/zymoscope/`
- `hardware/fermentabot-v2.kicad_pro` — replaced by `hardware/zymoscope.kicad_pro`
- `hardware/fermentabot-v2.kicad_sch` — replaced by `hardware/zymoscope.kicad_sch`
- `hardware/bom/fermentabot-v2-bom.csv` — replaced by `hardware/bom/zymoscope-bom.csv`

## License

GPL-3.0 (software), CERN-OHL-S-2.0 (hardware)
