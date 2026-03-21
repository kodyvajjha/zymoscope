# FermentaBot v2 — Prototype Build Guide

## Overview

This is the **minimum viable prototype**: an ESP32 dev board + breakout sensor
modules wired on a protoboard. No custom PCB needed. Total cost: **~$40–50**
depending on supplier.

The goal is to validate the sensor pipeline, firmware, and web app before
committing to a PCB revision.

---

## Parts List

### Core (order from Mouser or DigiKey)

| # | Part | Mouser PN | Qty | ~Price | Notes |
|---|------|-----------|-----|--------|-------|
| 1 | **ESP32-DevKitC-32E** | 356-ESP32DEVKITC32E | 1 | $10.00 | USB-C variant preferred. Any ESP32 dev board with GPIO breakout works. |
| 2 | **DS18B20 waterproof probe** (1m cable) | 700-DS18B20+ | 1 | $3.50 | Get the pre-wired stainless steel probe version. The bare TO-92 works too but the probe is better for liquids. |
| 3 | **4.7 kΩ resistor** (1/4W axial) | 603-MFR-25FBF52-4K7 | 1 | $0.10 | 1-Wire pull-up. Grab a 5-pack. |
| 4 | **10 kΩ resistor** (1/4W axial) | 603-MFR-25FBF52-10K | 2 | $0.10 | Pull-ups. Grab a 5-pack. |

**Mouser subtotal: ~$14**

### Breakout Modules (cheaper on Amazon or AliExpress)

These are commodity breakout boards. Mouser sells some of them but at 3–5x the
price. Amazon 2-day shipping is the sweet spot for prototyping.

| # | Part | Where to Buy | Qty | ~Price | Notes |
|---|------|-------------|-----|--------|-------|
| 5 | **HX711 + 5 kg load cell kit** | Amazon / AliExpress | 1 | $8–10 | Search "HX711 load cell kit 5kg". Comes with the HX711 breakout board + aluminum bar load cell + wires. This is your gravity sensor. |
| 6 | **BME280 breakout** (I2C, 3.3V) | Amazon / AliExpress | 1 | $3–5 | Search "BME280 breakout I2C". **Make sure it's a BME280, not BMP280** — the BME has humidity. 4-pin (VCC, GND, SCL, SDA). |
| 7 | **SSD1306 0.96" OLED** (I2C, 128×64) | Amazon / AliExpress | 1 | $3–4 | Search "SSD1306 0.96 I2C OLED". 4-pin version (VCC, GND, SCL, SDA). White or blue, doesn't matter. |
| 8 | **2-channel 5V relay module** (opto-isolated) | Amazon | 1 | $4–5 | Search "2 channel 5V relay module optocoupler". Has screw terminals for AC loads. High/low trigger selectable. |
| 9 | **Half-size breadboard** (400 tie points) | Amazon | 1 | $2–3 | Or a 7×9 cm solderable protoboard if you want something permanent. |
| 10 | **Dupont jumper wires** (M-M, M-F) | Amazon | 1 pack | $3–4 | 40-pin ribbon, get both male-male and male-female. |
| 11 | **Micro USB or USB-C cable** | — | 1 | $0 | You probably have one. Powers the ESP32 + provides serial for flashing. |

**Amazon subtotal: ~$25–30**

---

## What You Probably Already Have

- A laptop with USB (for flashing firmware)
- A 5V USB power supply / phone charger (1A+ is fine for prototyping)
- Something to ferment (beer, kombucha, whatever — or just a jar of warm water to test with)

## What You Do NOT Need Yet

- Custom PCB (that's phase 2, after we validate the prototype)
- Raspberry Pi (the web app runs on your laptop for now)
- pH sensor (adds analog complexity — save for v2.1)
- Barrel jack / DIN rail enclosure (prototype lives on the bench)
- Pi Camera (future phase)

---

## Wiring Guide

Once parts arrive, wire it up like this:

```
ESP32 DevKit Pin    →   Peripheral
──────────────────────────────────────────────────
GPIO 4              →   DS18B20 DATA (yellow wire)
                        + 4.7 kΩ pull-up to 3.3V
3.3V                →   DS18B20 VCC (red wire)
GND                 →   DS18B20 GND (black wire)

GPIO 18             →   HX711 SCK
GPIO 19             →   HX711 DT (DOUT)
3.3V                →   HX711 VCC
GND                 →   HX711 GND
                        Load cell wires → HX711 E+/E-/A+/A-
                        (red→E+, black→E-, white→A+, green→A-)

GPIO 21 (SDA)       →   BME280 SDA  +  OLED SDA  (shared I2C bus)
GPIO 22 (SCL)       →   BME280 SCL  +  OLED SCL  (shared I2C bus)
3.3V                →   BME280 VCC  +  OLED VCC
GND                 →   BME280 GND  +  OLED GND
                        (BME280 default addr: 0x76)
                        (SSD1306 default addr: 0x3C)

GPIO 25             →   Relay module IN1 (heater)
GPIO 26             →   Relay module IN2 (cooler)
5V (VIN pin)        →   Relay module VCC
GND                 →   Relay module GND
                        (Set relay jumper to LOW-trigger if available)
```

### Load Cell Mounting (for gravity estimation)

For prototyping, the simplest setup:

1. Screw the load cell to a flat board (wood or acrylic) at one end
2. Attach a small platform on the free end (zip-tie a flat piece)
3. Place your fermentation vessel on the platform
4. Tare after placing the full vessel — you're measuring *weight loss* from CO2 off-gassing

A 5 kg cell works for vessels up to ~5 kg total (≈1 gallon batch + container).
For 5-gallon batches, get a 20 kg load cell instead (~$2 more).

---

## Order Checklist

Copy-paste for ordering:

```
Mouser / DigiKey:
  [ ] ESP32-DevKitC-32E (or any ESP32 dev board)     ×1
  [ ] DS18B20 waterproof probe (pre-wired, 1m)        ×1
  [ ] 4.7 kΩ 1/4W resistor (5-pack)                   ×1
  [ ] 10 kΩ 1/4W resistor (5-pack)                     ×1

Amazon:
  [ ] HX711 + 5 kg load cell kit                       ×1
  [ ] BME280 I2C breakout (NOT BMP280)                  ×1
  [ ] SSD1306 0.96" OLED I2C 128×64                     ×1
  [ ] 2-channel 5V relay module (opto-isolated)         ×1
  [ ] Half-size breadboard (or 7×9 cm protoboard)       ×1
  [ ] Dupont jumper wires (M-M + M-F, 40-pin)          ×1

Total: ~$40–50
```

---

## Next Steps After Parts Arrive

1. Wire everything per the diagram above
2. Flash the firmware: `cd firmware/ && idf.py build flash monitor`
3. Start the web app: `cd dashboard/ && python -m fermentabot.server`
4. Open `http://localhost:8080` — you should see live sensor data
5. Calibrate the load cell with a known weight
6. Stick the temp probe in your vessel and start a batch
