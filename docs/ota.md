# Over-the-air (OTA) firmware updates

The ESP32 can update itself over Wi-Fi. A laptop serves the new image over
HTTP and sends the device a URL via MQTT. The device downloads the image into
its spare slot and reboots into it. If the new image can't reach the MQTT
broker, the device rolls back automatically.

```
laptop                       Pi (Mosquitto)                ESP32
 ota_push.py ──{"ota_url"}──▶ zymoscope/<id>/cmd ────────▶  ota task
     ▲  HTTP GET /zymoscope.bin ◀───────────────────────────  │ writes inactive slot
     └──── progress ◀──────── zymoscope/<id>/status ◀───────  │ reboots → verifies
```

## One-time setup (USB)

OTA needs a different partition table (two app slots) and a bootloader with
rollback support. A running app can't install either of those, so the first
OTA-capable build has to be flashed over USB once:

```bash
cd firmware/
. ~/esp/esp-idf/export.sh
esptool.py -p /dev/ttyUSB0 flash_id        # must report "Detected flash size: 4MB"
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor       # NOT erase-flash
```

The `nvs` partition keeps its old offset and size, so the stored setpoint and
Wi-Fi credentials survive. Don't run `idf.py erase-flash`, because that wipes them.

## Every update after that

```bash
cd firmware/
idf.py build
python tools/ota_push.py                   # auto-detects the device
```

Options: `--device <mac-hex>`, `--broker 10.0.0.102` (default), `--http-port 8070`.
The laptop must be on the same LAN as the ESP32, with port 8070 reachable.
The download takes about 30–60 s, plus about 2 min for the new image to verify.

## Safety behaviour

- **Wrong image:** if the image's project name isn't `zymoscope`, the device
  rejects it before writing anything.
- **Rollback:** a new image boots in `pending_verify` state. It's marked valid
  only after it has run for 2 minutes and published telemetry, which proves
  sensors, control and MQTT all work, so it can receive the next OTA. If it
  crashes or reboots before then, or hasn't verified within 5 minutes
  (`OTA_VERIFY_MIN_UPTIME_S` / `OTA_VERIFY_TIMEOUT_S` in `ota.h`), the
  bootloader boots the previous image.
- **Relays:** both relays are forced off just before every OTA or rollback
  reboot. The dashboard's plug watchdog covers a device that goes silent.
- **During download:** control keeps running. You may see a few DS18B20 CRC
  warnings while flash is being written. These are harmless, because a failed
  read turns the relays off for that cycle.
- No signing or TLS: anyone who can publish to the broker can push firmware.
  That's the same trust level as the rest of the unauthenticated LAN MQTT setup.

## Status topic

`zymoscope/<id>/status` (JSON):

| event     | when | fields |
|-----------|------|--------|
| `online`  | every MQTT connect (retained) | `fw`, `partition`, `ota_state`, `idf` |
| `offline` | last will (retained) | — |
| `ota`     | during an update | `state` (starting / downloading / rebooting / failed / rejected), `progress`, `detail` |

To check what's running without the tool:
`mosquitto_sub -h 10.0.0.102 -t 'zymoscope/+/status' -v -W 2`

Telemetry also carries `fw`, which the dashboard shows on the device card.
