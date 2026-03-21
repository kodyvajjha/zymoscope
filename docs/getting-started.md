# FermentaBot v2 — Getting Started (Step by Step)

Everything from ordering parts to watching your first fermentation curve on
the dashboard.

---

## Step 1: Order Parts

Place two orders. The Mouser/DigiKey parts are precision components with
exact part numbers. The Amazon parts are generic breakout boards — any
equivalent works.

### Mouser order (~$14, ships in 1–2 days)

Go to [mouser.com](https://www.mouser.com). Add each part number to your cart:

| Part | Mouser Part Number | Qty |
|------|--------------------|-----|
| ESP32-DevKitC-32E dev board | **356-ESP32DEVKITC32E** | 1 |
| DS18B20 temperature probe | **700-DS18B20+** | 1 |
| 4.7 kΩ resistor, 1/4W | **603-MFR-25FBF52-4K7** | 5 (pack) |
| 10 kΩ resistor, 1/4W | **603-MFR-25FBF52-10K** | 5 (pack) |

> **Note on the DS18B20:** Mouser sells the bare TO-92 chip. For fermentation
> you want the **waterproof stainless steel probe version** with pre-wired
> leads (red/yellow/black). Amazon has these for ~$3–5. Search
> "DS18B20 waterproof probe". Either works — the bare chip is fine for bench
> testing, but you'll want the probe for actual vessels.

### Amazon order (~$25–30, ships in 1–2 days with Prime)

Search for each item. These are commodity parts — any seller works.

| Part | What to Search | Qty | Tips |
|------|---------------|-----|------|
| HX711 + load cell kit | "HX711 load cell kit 5kg" | 1 | Must include the HX711 breakout board AND the aluminum bar-style load cell. Usually comes as a bundle for $8–10. |
| BME280 breakout | "BME280 breakout board I2C 3.3V" | 1 | **IMPORTANT: verify it says BME280, not BMP280.** The BMP lacks the humidity sensor. Should be a small purple or blue board with 4 pins (VCC, GND, SCL, SDA). |
| SSD1306 OLED | "SSD1306 0.96 inch OLED I2C 128x64" | 1 | 4-pin I2C version (not SPI). Color doesn't matter — white or blue. |
| 2-channel relay module | "2 channel 5V relay module optocoupler arduino" | 1 | Opto-isolated, screw terminals on the output side. Should have a VCC/GND/IN1/IN2 header. |
| Half-size breadboard | "solderless breadboard 400 point" | 1 | Or a 7×9 cm solderable protoboard if you want something more permanent. |
| Jumper wires | "dupont jumper wire kit male male female" | 1 | 40-pin ribbon. Get a pack with both male-male and male-female wires. |

### What you probably already have

- USB-A to Micro USB or USB-C cable (for the ESP32)
- A laptop (Linux, macOS, or Windows — for flashing and running the dashboard)
- A 5V USB power supply or phone charger (1A+)

### Total: ~$40–50

---

## Step 2: Install Software (while you wait for parts)

### 2a: Install ESP-IDF (firmware toolchain)

Follow Espressif's official guide for your OS:
https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/

**Linux (quick version):**

```bash
sudo apt install git wget flex bison gperf python3 python3-venv \
  cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0

mkdir -p ~/esp
cd ~/esp
git clone --recursive https://github.com/espressif/esp-idf.git -b v5.2.2
cd esp-idf
./install.sh esp32
source export.sh    # run this in every new terminal, or add to .bashrc
```

**macOS:**

```bash
brew install cmake ninja dfu-util python3
mkdir -p ~/esp && cd ~/esp
git clone --recursive https://github.com/espressif/esp-idf.git -b v5.2.2
cd esp-idf
./install.sh esp32
source export.sh
```

Verify it works:

```bash
idf.py --version
# Should print something like: ESP-IDF v5.2.2
```

### 2b: Install Python dependencies (web dashboard)

```bash
cd /path/to/fermentabot-v2/dashboard
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

### 2c: Install an MQTT broker

The web app and ESP32 communicate via MQTT. You need a broker running
somewhere on your network. The easiest option:

**Option A — Docker (recommended):**

```bash
docker run -d --name mosquitto \
  -p 1883:1883 \
  eclipse-mosquitto:2 \
  mosquitto -c /mosquitto-no-auth.conf
```

**Option B — Install directly:**

```bash
# Debian/Ubuntu
sudo apt install mosquitto mosquitto-clients
sudo systemctl enable --now mosquitto

# macOS
brew install mosquitto
brew services start mosquitto
```

**Option C — Use the included Docker Compose stack:**

```bash
cd dashboard/
cp .env.example .env
docker compose up -d mosquitto
```

Verify the broker is running:

```bash
# In one terminal:
mosquitto_sub -h localhost -t "test/#"

# In another:
mosquitto_pub -h localhost -t "test/hello" -m "it works"
# You should see "it works" appear in the first terminal
```

---

## Step 3: Wire the Hardware

Unpack everything and set it up on the breadboard. Work through one
peripheral at a time — it's easier to debug that way.

### 3a: Place the ESP32 dev board

Straddle it across the center channel of the breadboard so the pins go
into both sides. The USB port should face outward for easy access.

### 3b: Wire the DS18B20 temperature probe

```
DS18B20 wire color    →   ESP32
──────────────────────────────────
Red (VCC)             →   3.3V
Black (GND)           →   GND
Yellow (DATA)         →   GPIO 4

Also: place the 4.7 kΩ resistor between GPIO 4 and 3.3V
      (this is the 1-Wire pull-up — required)
```

If you have the bare TO-92 chip instead of a probe: pin 1 (left, flat
side facing you) = GND, pin 2 (center) = DATA, pin 3 (right) = VCC.

### 3c: Wire the HX711 + load cell

First, connect the load cell wires to the HX711 breakout board:

```
Load cell wire    →   HX711 board
──────────────────────────────────
Red               →   E+
Black             →   E-
White             →   A+
Green             →   A-
```

Then connect the HX711 breakout to the ESP32:

```
HX711 pin         →   ESP32
──────────────────────────────────
VCC               →   3.3V
GND               →   GND
SCK               →   GPIO 18
DT (DOUT)         →   GPIO 19
```

### 3d: Wire the BME280 (ambient sensor)

```
BME280 pin        →   ESP32
──────────────────────────────────
VCC               →   3.3V
GND               →   GND
SDA               →   GPIO 21
SCL               →   GPIO 22
```

### 3e: Wire the SSD1306 OLED

Shares the same I2C bus as the BME280 — wire to the same GPIO 21 / 22:

```
OLED pin          →   ESP32
──────────────────────────────────
VCC               →   3.3V
GND               →   GND
SDA               →   GPIO 21  (same wire row as BME280 SDA)
SCL               →   GPIO 22  (same wire row as BME280 SCL)
```

Both devices coexist on the same bus (BME280 @ address 0x76, OLED @ 0x3C).

### 3f: Wire the relay module

```
Relay module pin  →   ESP32
──────────────────────────────────
VCC               →   5V (the "VIN" pin on the ESP32 dev board)
GND               →   GND
IN1               →   GPIO 25  (heater control)
IN2               →   GPIO 26  (cooler control)
```

> **Important:** The relay module VCC goes to the ESP32's **5V/VIN pin** (not
> 3.3V). The relay coils need 5V. The signal pins (IN1/IN2) are fine at 3.3V
> logic. Don't connect AC mains loads yet — just test with the relay clicking
> for now.

### 3g: Double-check

Before powering on:

- [ ] All GND wires go to the same ground rail
- [ ] 3.3V and 5V are NOT shorted together
- [ ] The 4.7 kΩ resistor is between GPIO 4 and 3.3V
- [ ] I2C SDA/SCL wires from BME280 and OLED both go to GPIO 21/22
- [ ] No loose jumper wires touching things they shouldn't

---

## Step 4: Configure and Flash the Firmware

### 4a: Set your Wi-Fi credentials

Edit `firmware/main/comms/wifi_sta.h` and change the defaults:

```c
#ifndef WIFI_SSID
#define WIFI_SSID "YourWiFiName"       // <── change this
#endif

#ifndef WIFI_PASS
#define WIFI_PASS "YourWiFiPassword"   // <── change this
#endif
```

### 4b: Set your MQTT broker address

Edit `firmware/main/app_main.c` and change the broker URI:

```c
#ifndef MQTT_BROKER_URI
#define MQTT_BROKER_URI "mqtt://192.168.1.100:1883"  // <── your broker IP
#endif
```

Use the IP address of the machine running Mosquitto. Find it with:

```bash
# Linux
hostname -I

# macOS
ipconfig getifaddr en0
```

### 4c: Build and flash

Plug the ESP32 into your laptop via USB.

```bash
# Make sure ESP-IDF is sourced
source ~/esp/esp-idf/export.sh

cd /path/to/fermentabot-v2/firmware

idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

> **Port notes:**
> - Linux: usually `/dev/ttyUSB0` or `/dev/ttyACM0`
> - macOS: `/dev/cu.usbserial-*` or `/dev/cu.SLAB_USBtoUART`
> - If permission denied on Linux: `sudo usermod -aG dialout $USER` then log out/in

You should see log output like:

```
I (520) fermentabot: FermentaBot v2 starting...
I (530) fermentabot: Device MAC: AA:BB:CC:DD:EE:FF
I (780) wifi_sta: Connecting to "YourWiFiName"...
I (2100) wifi_sta: Connected! IP: 192.168.1.42
I (3200) mqtt: Connected to mqtt://192.168.1.100:1883
I (3210) fermentabot: Taring scale...
I (5200) fermentabot: DS18B20[0]: 22.31 C
I (5200) fermentabot: BME280: 23.1 C, 45.2% RH, 1013.2 hPa
I (5200) fermentabot: Weight: 0.0 g
```

The OLED should light up showing temperature, gravity, and relay states.

Press `Ctrl+]` to exit the serial monitor.

---

## Step 5: Start the Web Dashboard

In a new terminal on your laptop:

```bash
cd /path/to/fermentabot-v2/dashboard
source .venv/bin/activate

# Point it at your MQTT broker (skip if running on the same machine)
export MQTT_BROKER=localhost

python -m fermentabot.server
```

Open **http://localhost:8000** in your browser.

You should see:
- A dark-themed dashboard
- A card for your ESP32 device (identified by its MAC address)
- Live temperature, estimated gravity, humidity, pressure
- Relay state indicators (green/red dots)
- A line chart updating every 30 seconds with temp and gravity

If you don't see a device card, check:
1. Is the ESP32 connected to Wi-Fi? (check serial monitor output)
2. Is the MQTT broker running?
3. Is the dashboard's `MQTT_BROKER` env var pointing to the right host?

---

## Step 6: Mount the Load Cell

This is the gravity estimation sensor. It measures weight loss as CO2
escapes during fermentation.

### Materials

- The aluminum bar load cell from the HX711 kit
- A flat board (wood, acrylic, or thick cardboard — at least 20 cm long)
- 2 screws (M4 or M5, usually included in the kit)
- Something flat for a platform (a small cutting board, piece of acrylic, etc.)

### Assembly

```
       ┌──────────────────────────────────────────────┐
       │              flat base board                   │
       │                                                │
       │   ┌─ screws ─┐                                │
       │   ▼          ▼    ┌─── load cell bar ───┐     │
       │   ██████████████████████████████████████  │     │
       │   ▲ (fixed end)   ▲ (free end + platform)│     │
       │                    │                      │     │
       │                    └── small platform ────┘     │
       │                        (vessel sits here)       │
       └─────────────────────────────────────────────────┘
```

1. Screw the **fixed end** (the end with the arrow pointing toward it, or the
   end closest to the wires) to one end of the base board
2. Attach a small flat platform to the **free end** with screws, zip-ties, or
   strong tape — this is where the vessel sits
3. Make sure the load cell can flex freely. It should not touch the base board
   when loaded

### Calibration

After mounting, you need to calibrate the scale factor:

1. Power on the ESP32 — it auto-tares on boot (reads with nothing on the
   platform as zero)
2. Place a known weight on the platform (e.g., a 500 ml water bottle = ~500 g)
3. Watch the serial monitor — note the reported weight
4. If it's off, adjust the scale factor in `firmware/main/sensor/hx711.c`
   (look for `HX711_DEFAULT_SCALE`) and re-flash

For the prototype, rough calibration is fine. You're tracking *relative weight
loss*, so absolute accuracy matters less than consistency.

---

## Step 7: Start a Fermentation

### 7a: Prepare your vessel

Whatever you're fermenting — beer, kombucha, cider, mead, hot sauce, etc.
A 1-gallon glass jug or a small fermentation bucket works well for the
prototype.

### 7b: Position the sensors

1. Place the vessel on the load cell platform
2. Dip the DS18B20 probe into the liquid (use the airlock hole or tape it
   to the outside of the vessel for external temp)
3. The BME280 stays on the breadboard — it reads ambient room conditions
4. The OLED sits on the breadboard — it shows live status

### 7c: Power on and tare

1. Plug in the ESP32 via USB
2. Wait for the boot sequence — it will auto-tare the scale
3. Check the serial monitor to confirm readings look sane
4. Check the web dashboard — you should see the device card with live data

### 7d: Create a batch in the dashboard

Open http://localhost:8000 and use the "New Batch" form:

- **Name:** something like "IPA batch 3" or "kombucha round 2"
- **Style:** "IPA", "Kombucha", "Cider", etc.
- **OG:** your original gravity reading (from a hydrometer, if you have one)

### 7e: Walk away

The system now runs autonomously:

- Temperature is logged every 5 seconds
- Telemetry is published to MQTT every 30 seconds
- The web dashboard stores and plots everything
- If you wired up a heater/cooler to the relay screw terminals, the PID
  controller maintains your setpoint automatically
- Weight loss is tracked — as CO2 escapes, the gravity estimate drops

Check in on the dashboard periodically. A typical ale fermentation shows:
- Active phase (days 1–3): temp rises from yeast activity, weight drops
  rapidly as CO2 is produced
- Slowing phase (days 4–7): weight loss slows, temp stabilizes
- Terminal (day 7+): weight and gravity plateau — fermentation is done

---

## Step 8: Adjust the Temperature Setpoint (optional)

If you have a heater or cooler connected to the relays, you can change the
PID setpoint remotely via MQTT:

```bash
# Find your device MAC (shown in the serial monitor or dashboard)
# Replace AA:BB:CC:DD:EE:FF with your actual MAC (lowercase, no colons)

mosquitto_pub -h localhost \
  -t "fermentabot/aabbccddeeff/cmd" \
  -m '{"setpoint": 18.0}'
```

Or use the dashboard's command endpoint:

```bash
curl -X POST http://localhost:8000/api/cmd/aabbccddeeff \
  -H "Content-Type: application/json" \
  -d '{"setpoint": 18.0}'
```

---

## Troubleshooting

| Problem | Check |
|---------|-------|
| ESP32 won't flash | Correct USB port? Try holding BOOT button while flashing. Run `ls /dev/tty*` to find the port. |
| No temperature readings | Is the 4.7 kΩ pull-up resistor in place? Check DS18B20 wire colors. |
| OLED blank | Is it wired to GPIO 21/22? Check the I2C address — some OLEDs use 0x3D instead of 0x3C. |
| BME280 reads 0 | Make sure it's a BME280, not BMP280. Check I2C wiring (SDA/SCL not swapped). |
| HX711 reads garbage | Check load cell wire order (red→E+, black→E-, white→A+, green→A-). These vary by manufacturer — try swapping white and green if readings are unstable. |
| Wi-Fi won't connect | Check SSID/password in wifi_sta.h. ESP32 only supports 2.4 GHz networks. |
| Dashboard shows no devices | Is Mosquitto running? Is the ESP32 connected to the same network? Check `MQTT_BROKER` env var. |
| Relay clicks but nothing happens | Check relay module VCC goes to 5V (VIN), not 3.3V. Verify IN1/IN2 trigger polarity (some modules are active-low). |
| Weight drifts over time | Normal for cheap load cells. Thermal drift is the main cause. Let the cell warm up for 30 min before taring. |

---

## What's Next

Once your prototype is running and tracking a fermentation:

- **Add more DS18B20 probes** — the firmware supports up to 4 on the same bus.
  Just wire additional probes to the same GPIO 4 + 3.3V + GND lines.
- **Connect actual heating/cooling** — wire a heat wrap or mini fridge plug
  through the relay screw terminals (be careful with mains voltage!).
- **Run it on a Pi** — move the dashboard to a Raspberry Pi with a small
  screen for a dedicated brewing station.
- **Add a camera** — a Pi Camera Module can track krausen height and color.
  This is the planned Phase 3.
