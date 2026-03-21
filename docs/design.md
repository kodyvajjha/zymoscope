# FermentaBot v2 — Design Document

## 1. Goals and Constraints

| Goal | Rationale |
|------|-----------|
| Self-hosted telemetry | No cloud dependency for reliability |
| ±0.1 °C temperature accuracy | Tight lager fermentation window |
| BLE tilt sensor support | Passive gravity readings without opening vessel |
| Sub-$30 BOM per unit | Hobbyist replicability |
| DIN-rail enclosure | Clean brewery panel mounting |

## 2. System Architecture

```
┌─────────────────────────────────────────────────────┐
│  Fermentation Vessel                                │
│  ┌──────────┐   ┌──────────┐   ┌────────────────┐  │
│  │ DS18B20  │   │  Tilt    │   │ CO2 pressure   │  │
│  │ (1-Wire) │   │ (BLE RX) │   │ sensor (I²C)   │  │
│  └────┬─────┘   └────┬─────┘   └───────┬────────┘  │
└───────┼──────────────┼─────────────────┼────────────┘
        │              │                 │
┌───────▼──────────────▼─────────────────▼────────────┐
│  FermentaBot PCB                                    │
│  ┌────────────────────────────────────────────────┐ │
│  │  ESP32-WROOM-32E                               │ │
│  │  ┌──────────┐  ┌──────────┐  ┌─────────────┐  │ │
│  │  │ 1-Wire   │  │  BLE     │  │  I²C master │  │ │
│  │  │ driver   │  │  scanner │  │             │  │ │
│  │  └────┬─────┘  └──────────┘  └──────┬──────┘  │ │
│  │       │   ┌─────────────────────────┘         │ │
│  │       └───►  Sensor fusion / state machine     │ │
│  │             │                                  │ │
│  │             ▼                                  │ │
│  │         MQTT publish ──► Wi-Fi ──► broker      │ │
│  │         OLED update                            │ │
│  │         Relay control (GPIO)                   │ │
│  └────────────────────────────────────────────────┘ │
│  ┌────────────┐  ┌───────────────────────────────┐  │
│  │ AMS1117    │  │ Relay ×2 (heater / cooler)    │  │
│  │ 3.3V LDO   │  │ 5V coil, opto-isolated        │  │
│  └────────────┘  └───────────────────────────────┘  │
└─────────────────────────────────────────────────────┘
                        │ MQTT
                        ▼
┌─────────────────────────────────────────────────────┐
│  Self-hosted stack (Raspberry Pi / VM)              │
│  Mosquitto ──► Telegraf ──► InfluxDB ──► Grafana    │
│                                 └──► Alert webhooks │
└─────────────────────────────────────────────────────┘
```

## 3. Hardware Design

### 3.1 MCU Selection — ESP32-WROOM-32E

- Dual-core Xtensa LX6 @ 240 MHz; plenty of headroom for BLE scanning + MQTT
- Integrated Wi-Fi (802.11 b/g/n) + BLE 4.2
- 4 MB flash; 520 KB SRAM
- Wide supply range (3.0–3.6 V); efficient at 80 MHz for always-on monitoring

### 3.2 Power Supply

- Input: 5 V DC barrel jack (2.1 mm center-positive)
- 3.3 V rail: AMS1117-3.3 LDO, 800 mA max
- Relay coils driven from 5 V rail directly (before LDO)
- ESP32 EN pin pulled high via 10 kΩ; boot strapping resistors per Espressif ref design

### 3.3 Temperature Sensing — DS18B20

- Parasitic power mode, 1-Wire on GPIO4
- 4.7 kΩ pull-up to 3.3 V
- 12-bit resolution → 0.0625 °C step; conversion time 750 ms max
- Up to 4 probes on one bus (vessel, ambient, cold side, hot side)

### 3.4 Display — SSD1306 128×64 OLED

- I²C at 0x3C, 400 kHz fast mode
- Shows: current temp, setpoint, gravity estimate, elapsed days, Wi-Fi status
- GPIO21 = SDA, GPIO22 = SCL

### 3.5 Relay Outputs

- Two SPDT relays, 10 A / 250 VAC contacts
- Opto-isolated driver (PC817); GPIO driven via NPN transistor (2N2222)
- Flyback diode on coil (1N4148)
- Output 1 — Heater (heating pad / heat wrap)
- Output 2 — Cooler (mini fridge or Peltier module)

### 3.6 Connectors

| Connector | Type | Signals |
|-----------|------|---------|
| J1 | 2-pin 5.08 mm screw terminal | 5 V in, GND |
| J2 | 3-pin 3.81 mm screw terminal | 1-Wire (DATA, GND, VCC) |
| J3 | 2-pin 5.08 mm screw terminal | Relay 1 NO/COM |
| J4 | 2-pin 5.08 mm screw terminal | Relay 2 NO/COM |
| J5 | 4-pin 2.54 mm header | UART0 (TX, RX, GND, 3V3) for flashing |

## 4. Firmware Architecture

```
main/
├── app_main.c          Entry point, task creation
├── sensor/
│   ├── ds18b20.c       1-Wire driver
│   └── tilt_ble.c      BLE passive scanner (iBeacon / Tilt UUID)
├── control/
│   └── pid.c           Simple PID for temperature setpoint
├── comms/
│   ├── mqtt_client.c   Publish telemetry JSON
│   └── wifi_sta.c      Wi-Fi init + reconnect
├── display/
│   └── oled.c          SSD1306 I²C driver + layout
└── config/
    └── nvs_config.c    Persist setpoint + credentials to NVS
```

### 4.1 MQTT Topic Schema

```
fermentabot/<device_id>/telemetry   JSON, published every 30 s
fermentabot/<device_id>/cmd         Subscribe for setpoint / relay override
fermentabot/<device_id>/status      LWT "offline" / "online"
```

Telemetry payload example:
```json
{
  "ts": 1710000000,
  "temp_c": [20.1, 19.8],
  "gravity_sg": 1.048,
  "relay": [0, 1],
  "rssi": -62,
  "uptime_s": 86400
}
```

## 5. Dashboard Stack

| Service | Image | Port |
|---------|-------|------|
| Mosquitto | eclipse-mosquitto:2 | 1883, 8883 |
| Telegraf | telegraf:1.30 | — |
| InfluxDB | influxdb:2.7 | 8086 |
| Grafana | grafana/grafana-oss:10.4 | 3000 |

Grafana dashboards are provisioned via YAML in `dashboard/grafana/provisioning/`.

## 6. Open Items / Future Work

- [ ] Add flow meter for sparge water (v3)
- [ ] OTA firmware update via HTTPS
- [ ] PCB revision: add TVS diodes on relay output terminals
- [ ] Evaluate CO2 volume measurement via pressure + temp logging
- [ ] Mobile-friendly Grafana layout
