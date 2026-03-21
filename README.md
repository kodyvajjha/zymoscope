# FermentaBot v2

An ESP32-based fermentation monitoring and control system with a real-time web dashboard.

## Overview

FermentaBot v2 monitors and controls fermentation environments using:

- **Temperature** sensing and PID control (DS18B20 probes + relay-driven heating/cooling)
- **Specific gravity** estimation via load cell weight loss (CO2 off-gassing tracks sugar consumption)
- **Ambient climate** logging (BME280 — temperature, humidity, pressure)
- **Local display** on a 128x64 OLED for at-a-glance status
- **Wi-Fi telemetry** via MQTT to a self-hosted FastAPI + SQLite web dashboard
- **Modular architecture** — ESP32 sensor nodes now, Raspberry Pi + camera later

Inspired by the [OpenAg FermentaBot](https://github.com/OpenAgricultureFoundation/fermentabot),
rebuilt from scratch with custom hardware, real sensor drivers, and a modern web frontend.

## Architecture

```
ESP32 Sensor Node (per vessel)        Web App (laptop / Pi / server)
┌────────────────────────────┐        ┌─────────────────────────────┐
│ DS18B20 ×4  (1-Wire)       │  MQTT  │ FastAPI backend             │
│ HX711 + load cell (weight) │───────>│ SQLite storage              │
│ BME280 (ambient)           │        │ WebSocket live push         │
│ SSD1306 OLED (status)      │<───────│ Chart.js dashboard          │
│ Relay ×2 (heat/cool)       │  cmds  │ Batch tracking              │
│ PID temperature control    │        └─────────────────────────────┘
└────────────────────────────┘
```

## Repository Layout

```
fermentabot-v2/
├── firmware/                   # ESP-IDF v5.x firmware (C)
│   ├── main/
│   │   ├── app_main.c          # Entry point, FreeRTOS tasks
│   │   ├── sensor/
│   │   │   ├── ds18b20.c/h     # 1-Wire bit-bang, multi-sensor, CRC-8
│   │   │   ├── hx711.c/h       # 24-bit ADC bit-bang, tare, calibration
│   │   │   └── bme280_i2c.c/h  # I2C driver, Bosch compensation formulas
│   │   ├── display/
│   │   │   └── oled.c/h        # SSD1306 128×64, built-in 8×8 font
│   │   ├── control/
│   │   │   └── pid.c/h         # PID with anti-windup, [-1,1] output
│   │   └── comms/
│   │       ├── wifi_sta.c/h    # Wi-Fi STA, auto-reconnect
│   │       └── mqtt_client.c/h # JSON telemetry + setpoint commands
│   ├── CMakeLists.txt
│   └── sdkconfig.defaults
├── dashboard/                  # Python web app
│   ├── fermentabot/
│   │   ├── server.py           # FastAPI routes + WebSocket
│   │   ├── db.py               # SQLite schema + queries (async)
│   │   ├── mqtt_sub.py         # MQTT subscriber thread
│   │   └── config.py           # Environment-based settings
│   ├── templates/
│   │   └── index.html          # Dark-themed SPA, Chart.js live graphs
│   ├── docker-compose.yml      # Optional: Mosquitto + InfluxDB + Grafana
│   └── requirements.txt
├── hardware/                   # KiCad schematic + BOM
│   ├── fermentabot-v2.kicad_pro
│   ├── fermentabot-v2.kicad_sch
│   └── bom/
│       └── fermentabot-v2-bom.csv
├── docs/
│   ├── design.md               # Full architecture + design rationale
│   └── prototype-bom.md        # Ordering guide + wiring diagram
├── .gitignore
├── LICENSE                     # GPL-3.0
└── README.md
```

## Prototype Build (~$40–50)

No custom PCB needed. Use an ESP32 dev board + breakout modules on a breadboard.

### Order from Mouser / DigiKey (~$14)

| Part | Mouser PN | Qty | ~Price |
|------|-----------|-----|--------|
| ESP32-DevKitC-32E | 356-ESP32DEVKITC32E | 1 | $10.00 |
| DS18B20 waterproof probe | 700-DS18B20+ | 1 | $3.50 |
| 4.7 kΩ resistor (1/4W) | 603-MFR-25FBF52-4K7 | 1 | $0.10 |
| 10 kΩ resistor (1/4W) | 603-MFR-25FBF52-10K | 2 | $0.10 |

### Order from Amazon (~$25–30)

| Part | Search Term | Qty | ~Price |
|------|------------|-----|--------|
| HX711 + 5 kg load cell kit | "HX711 load cell kit 5kg" | 1 | $8–10 |
| BME280 I2C breakout | "BME280 breakout I2C" (NOT BMP280) | 1 | $3–5 |
| SSD1306 0.96" OLED 128×64 | "SSD1306 0.96 I2C OLED" | 1 | $3–4 |
| 2-ch 5V relay module | "2 channel 5V relay module optocoupler" | 1 | $4–5 |
| Half-size breadboard | "400 tie point breadboard" | 1 | $2–3 |
| Dupont jumper wires (M-M + M-F) | "dupont jumper wire kit" | 1 | $3–4 |

See [`docs/prototype-bom.md`](docs/prototype-bom.md) for the full wiring guide,
load cell mounting instructions, and detailed notes on each part.

## Quick Start

### Firmware

Requires [ESP-IDF v5.x](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/).

```bash
# Set your Wi-Fi credentials (edit defaults or flash via NVS)
# Edit firmware/main/comms/wifi_sta.c: WIFI_SSID / WIFI_PASS

cd firmware/
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

### Web Dashboard

```bash
cd dashboard/
pip install -r requirements.txt

# Optional: set MQTT broker address (default: localhost:1883)
export MQTT_BROKER=192.168.1.100

python -m fermentabot.server
# Open http://localhost:8080
```

The dashboard shows per-device cards with live temperature, estimated gravity,
ambient conditions, relay states, and 24-hour trend charts. Data persists in
SQLite. Batches can be created and tracked.

### Docker Stack (optional)

If you want Mosquitto + InfluxDB + Grafana alongside the web app:

```bash
cd dashboard/
cp .env.example .env   # edit secrets
docker compose up -d
```

## Wiring

```
ESP32 Pin     Peripheral
─────────     ──────────────────────────────────────────
GPIO 4        DS18B20 DATA + 4.7 kΩ pull-up to 3.3V
GPIO 18       HX711 SCK
GPIO 19       HX711 DOUT
GPIO 21       I2C SDA (BME280 @ 0x76 + OLED @ 0x3C)
GPIO 22       I2C SCL (BME280 + OLED)
GPIO 25       Relay IN1 (heater)
GPIO 26       Relay IN2 (cooler)
3.3V          DS18B20 VCC, BME280 VCC, OLED VCC, HX711 VCC
5V (VIN)      Relay module VCC
GND           Common ground
```

## MQTT Topics

```
fermentabot/<mac>/telemetry    # JSON, published every 30 s
fermentabot/<mac>/cmd          # Subscribe: {"setpoint": 20.0}
fermentabot/<mac>/status       # LWT: "online" / "offline"
```

## Roadmap

- [x] ESP32 sensor node firmware (DS18B20, HX711, BME280, OLED, PID, MQTT)
- [x] FastAPI web dashboard with live charts
- [ ] Raspberry Pi integration + camera module (krausen tracking)
- [ ] pH probe support (analog front-end)
- [ ] OTA firmware updates
- [ ] Fermentation profile automation (diacetyl rest, crash cool)
- [ ] Custom 2-layer PCB (KiCad, JLCPCB-ready)
- [ ] 3D-printable enclosure (OpenSCAD)

## License

This project is licensed under the **GNU General Public License v3.0**.
See [LICENSE](LICENSE) for details.

Hardware design files are additionally released under
**CERN Open Hardware Licence v2 – Strongly Reciprocal (CERN-OHL-S-2.0)**.
