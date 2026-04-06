# Zymoscope v2 — Getting Started (Step by Step)

Everything from ordering parts to watching your first fermentation curve on
the dashboard.

---

## Step 1: Order Parts

Place two orders. The Mouser/DigiKey parts are precision components with
exact part numbers. The Amazon parts are generic breakout boards — any
equivalent works.

### Mouser order (~$20, ships in 1–2 days)

Go to [mouser.com](https://www.mouser.com). Add each part number to your cart:

| Part | Mouser Part Number | Qty |
|------|--------------------|-----|
| ESP32-DevKitC-32E dev board | **356-ESP32DEVKITC32E** | 1 |
| DS18B20 temperature probe | **485-381** | 1 |
| 4.7 kΩ resistor, 1/4W | **603-MFR-25FBF52-4K7** | 5 (pack) |

> **Note:** This is the waterproof stainless steel probe version with
> pre-wired leads (red/yellow/black). Required for liquid immersion.

### Amazon order (~$25–35, ships in 1–2 days with Prime)

Search for each item. These are commodity parts — any seller works.

| Part | What to Search | Qty | Tips |
|------|---------------|-----|------|
| TP-Link Kasa KP115 smart plug | "TP-Link Kasa KP115" | 1 | The KP115 includes energy monitoring (power/voltage/current shown on the dashboard). KP125 or HS110 also work. ~$15–20. |
| BME280 breakout | "BME280 breakout board I2C 3.3V" | 1 | **IMPORTANT: verify it says BME280, not BMP280.** The BMP lacks the humidity sensor. Should be a small purple or blue board with 4 pins (VCC, GND, SCL, SDA). **Solder the header pins** — resting the board on unsoldered pins causes noise that corrupts the DS18B20. |
| SSD1306 OLED | "SSD1306 0.96 inch OLED I2C 128x64" | 1 | 4-pin I2C version (not SPI). Color doesn't matter — white or blue. **Solder header pins.** |
| Seedling heat mat | "seedling heat mat" | 1 | Plugs into the Kasa smart plug. The PID controller turns it on/off to maintain target temperature. |
| Breadboard | "solderless breadboard 830 point" or "400 tie point breadboard" | 1 | Full-size (830 point) is easiest for the ESP32-DevKitC. Half-size works if you run power/ground off-board. |
| Jumper wires | "dupont jumper wire kit male male female" | 1 | 40-pin ribbon. Get a pack with both male-male and male-female wires. |

### What you probably already have

- USB-A to Micro USB or USB-C cable (for the ESP32)
- A laptop (Linux, macOS, or Windows — for flashing and running the dashboard)
- A 5V USB power supply or phone charger (1A+)

### Optional extras

| Part | What to Search | Notes |
|------|---------------|-------|
| HX711 + 5 kg load cell kit | "HX711 load cell kit 5kg" | For gravity estimation via weight loss. Not needed for basic temperature control. |
| 2-channel relay module | "2 channel 5V relay module optocoupler" | For direct low-voltage DC control (fans, pumps, LED strips). Not needed if using the Kasa smart plug. |
| 10 kΩ resistor, 1/4W | 603-MFR-25FBF52-10K (Mouser) | Only needed if using the HX711. |

### Total: ~$40–55

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

**Arch Linux:**

```bash
sudo pacman -S git wget flex bison gperf python cmake ninja ccache \
  libffi openssl dfu-util libusb

mkdir -p ~/esp && cd ~/esp
git clone --recursive https://github.com/espressif/esp-idf.git -b v5.2.2
cd esp-idf
./install.sh esp32
source export.sh
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
cd /path/to/zymoscope/dashboard
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

### 2c: Install an MQTT broker

The web app and ESP32 communicate via MQTT. You need a broker running
somewhere on your network.

**Option A — Run directly (simplest):**

```bash
# Install
sudo pacman -S mosquitto          # Arch
sudo apt install mosquitto         # Debian/Ubuntu
brew install mosquitto             # macOS

# Run with the included config
mosquitto -c /path/to/zymoscope/dashboard/mosquitto/mosquitto.conf
```

**Option B — Docker:**

```bash
docker run -d --name mosquitto \
  -p 1883:1883 \
  eclipse-mosquitto:2 \
  mosquitto -c /mosquitto-no-auth.conf
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

### 2d: Set up the Kasa smart plug

1. Plug in the KP115 and set it up on your Wi-Fi using the Kasa app (one-time)
2. Find its IP address:

```bash
kasa discover
```

Note the IP address — you'll need it when starting the dashboard.

---

## Step 3: Wire the Hardware

Unpack everything and set it up on the breadboard. Work through one
peripheral at a time — it's easier to debug that way.

> **Important:** Solder header pins onto the BME280 and OLED modules before
> wiring. Resting them on unsoldered pins causes intermittent contact that
> corrupts sensor readings and produces CRC errors.

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

### 3c: Wire the BME280 (ambient sensor)

```
BME280 pin        →   ESP32
──────────────────────────────────
VCC               →   3.3V
GND               →   GND
SDA               →   GPIO 21
SCL               →   GPIO 22
```

> **Tip:** Run separate power jumpers from the ESP32 3.3V pin to each sensor
> rather than daisy-chaining them on one breadboard power rail. This reduces
> noise coupling between the BME280 and DS18B20.

### 3d: Wire the SSD1306 OLED

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

### 3e: Set up the Kasa smart plug

1. Plug the seedling heat mat into the Kasa smart plug
2. Plug the smart plug into the wall outlet
3. The dashboard controls the plug over Wi-Fi — no wiring to the ESP32

### 3f: Double-check

Before powering on:

- [ ] All GND wires go to the same ground rail
- [ ] 3.3V is not shorted to anything it shouldn't be
- [ ] The 4.7 kΩ resistor is between GPIO 4 and 3.3V
- [ ] I2C SDA/SCL wires from BME280 and OLED both go to GPIO 21/22
- [ ] BME280 and OLED header pins are **soldered**, not just resting
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

Find the IP of the machine running Mosquitto:

```bash
# Linux
ip addr show | grep 'inet 10\.\|inet 192\.'

# macOS
ipconfig getifaddr en0
```

Edit `firmware/main/app_main.c` and change the broker URI:

```c
#ifndef MQTT_BROKER_URI
#define MQTT_BROKER_URI "mqtt://192.168.1.100:1883"  // <── your broker IP
#endif
```

### 4c: Build and flash

Plug the ESP32 into your laptop via USB.

```bash
# Make sure ESP-IDF is sourced
source ~/esp/esp-idf/export.sh

cd /path/to/zymoscope/firmware

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
I (520) zymoscope: Zymoscope v2 starting...
I (530) zymoscope: Device MAC: AA:BB:CC:DD:EE:FF
I (845) wifi_sta: Connecting...
I (935) wifi:connected with YourWiFiName
I (875) bme280: BME280 initialised (addr=0x76, I2C port 0)
I (985) oled: SSD1306 128x64 OLED initialised (addr=0x3C)
I (1015) ds18b20: Found sensor 1: 2831A37B00000085
I (2655) mqtt: Connected to broker
```

The OLED should light up showing temperature, target setpoint, and connection
status. Press `Ctrl+]` to exit the serial monitor.

---

## Step 5: Start the Web Dashboard

### 5a: Start Mosquitto (if not already running)

```bash
mosquitto -c /path/to/zymoscope/dashboard/mosquitto/mosquitto.conf
```

### 5b: Start the dashboard

In a new terminal:

```bash
cd /path/to/zymoscope/dashboard
source .venv/bin/activate

# Set the Kasa plug IP
export KASA_HEATER_HOST=192.168.1.50    # your plug's IP from kasa discover

# Optional: for a cooler plug too
# export KASA_COOLER_HOST=192.168.1.51

python -m zymoscope.server
```

Open **http://localhost:8000** in your browser.

You should see:
- A dark-themed dashboard
- A card for your ESP32 device (identified by its MAC address)
- Live temperature and ambient readings
- Heater/cooler state indicators
- A line chart updating every 30 seconds
- "Target Temperature" control in the sidebar

If you don't see a device card, check:
1. Is the ESP32 connected to Wi-Fi? (check serial monitor output)
2. Is the MQTT broker running?
3. Is the ESP32's `MQTT_BROKER_URI` pointing to the right IP?

---

## Step 6: Set the Target Temperature

The PID controller maintains a target temperature by toggling the Kasa smart
plug (heater) on and off. The default is 20.0 °C.

### From the dashboard

Use the "Target Temperature" input in the sidebar. Enter a value and click
**Set**. The OLED will update to show the new target.

### From the command line

```bash
# Replace the device ID with yours (shown in serial monitor or dashboard)
curl -X POST http://localhost:8000/api/cmd/aabbccddeeff \
  -H "Content-Type: application/json" \
  -d '{"setpoint": 25.0}'
```

### Via MQTT directly

```bash
mosquitto_pub -h localhost \
  -t "zymoscope/aabbccddeeff/cmd" \
  -m '{"setpoint": 25.0}'
```

The setpoint is saved to the ESP32's NVS (non-volatile storage). It persists
across reboots — you can unplug the laptop and the ESP32 keeps controlling
temperature at the last setpoint.

---

## Step 7: Start a Fermentation

### 7a: Prepare your vessel

Whatever you're fermenting — beer, kombucha, cider, mead, hot sauce, etc.
A 1-gallon glass jug or a small fermentation bucket works well.

### 7b: Position the sensors

1. Dip the DS18B20 probe into the liquid (use the airlock hole or tape it
   to the outside of the vessel for external temp)
2. Place the seedling heat mat under or around the vessel, plugged into the
   Kasa smart plug
3. The BME280 stays on the breadboard — it reads ambient room conditions
4. The OLED sits on the breadboard — it shows live status

### 7c: Power on

1. Plug in the ESP32 via USB
2. Wait for the boot sequence
3. Check the serial monitor to confirm readings look sane
4. Check the web dashboard — you should see the device card with live data

### 7d: Walk away

The system now runs autonomously:

- Temperature is logged every 5 seconds
- Telemetry is published to MQTT every 30 seconds
- The PID controller toggles the heat mat via the Kasa plug to maintain
  your target temperature
- The web dashboard stores and plots everything (when running)
- The ESP32 + Kasa plug work independently of the dashboard

Check in on the dashboard periodically, or just glance at the OLED.

---

## Troubleshooting

| Problem | Check |
|---------|-------|
| ESP32 won't flash | Correct USB port? Try holding BOOT button while flashing. Run `ls /dev/tty*` to find the port. |
| No temperature readings | Is the 4.7 kΩ pull-up resistor in place? Check DS18B20 wire colors. |
| CRC mismatch errors | Solder the BME280/OLED header pins. Unsoldered pins cause noise on the shared power rail. Use separate 3.3V jumpers for each sensor. |
| OLED blank | Is it wired to GPIO 21/22? Check the I2C address — some OLEDs use 0x3D instead of 0x3C. Solder the pins. |
| BME280 reads 0.0 | Likely bad connection during init — solder pins, then reboot. Make sure it's a BME280, not BMP280. |
| Wi-Fi won't connect | Check SSID/password in wifi_sta.h. ESP32 only supports 2.4 GHz networks. |
| Dashboard shows no devices | Is Mosquitto running? Is the ESP32's MQTT_BROKER_URI correct? Check with `mosquitto_sub -t "zymoscope/#"`. |
| Kasa plug not found | Run `kasa discover`. Make sure the plug is on the same Wi-Fi network. Check KASA_HEATER_HOST env var. |
| Watchdog timer errors | Normal during early boot if many sensors are initializing. Should resolve after a few seconds. |
| MQTT keeps reconnecting | Check the Mosquitto log for "already connected" errors. Make sure only one client uses the same ID. |

---

## Optional: HX711 Load Cell (Gravity Estimation)

If you want to estimate specific gravity via weight loss during fermentation,
add the HX711 + load cell kit.

### Wiring

```
HX711 pin         →   ESP32
──────────────────────────────────
VCC               →   3.3V
GND               →   GND
SCK               →   GPIO 18
DT (DOUT)         →   GPIO 19
```

Load cell to HX711: Red→E+, Black→E-, White→A+, Green→A-.

### Calibration

1. Power on — the ESP32 auto-tares on boot
2. Place a known weight on the platform (e.g., 500 ml water = ~500 g)
3. Adjust `HX711_DEFAULT_SCALE` in `firmware/main/sensor/hx711.c` if needed

---

## Optional: Relay Module (Direct Control)

If you prefer direct low-voltage DC control (fans, pumps, LED strips) instead
of smart plugs:

### Wiring

```
Relay module pin  →   ESP32
──────────────────────────────────
VCC               →   5V (VIN pin on ESP32)
GND               →   GND
IN1               →   GPIO 25  (heater control)
IN2               →   GPIO 26  (cooler control)
```

> **Note:** The relay module VCC goes to **5V/VIN**, not 3.3V. Do not connect
> mains loads to the relay screw terminals unless you know what you're doing.
> For mains loads, use the Kasa smart plug instead.

---

## What's Next

Once your prototype is running and tracking a fermentation:

- **Add more DS18B20 probes** — the firmware supports up to 4 on the same bus.
  Just wire additional probes to the same GPIO 4 + 3.3V + GND lines.
- **Run it on a Pi** — move the dashboard to a Raspberry Pi with a small
  screen for a dedicated brewing station.
- **Add a camera** — a Pi Camera Module can track krausen height and color.
  This is the planned Phase 3.
