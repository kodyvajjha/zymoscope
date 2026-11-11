# Temperature control tuning

## What was wrong (before 2026-11-10)

The old loop computed PID once every 5 s, using gains that assumed a fixed
5 s step (Kp 2, Ki 0.1, Kd 0.5), and then thresholded the output at ±0.1. In
practice that made it an on/off thermostat:

- The integral saturated at ±1 within a few minutes on either side of the
  setpoint. With Kp = 2, that acted as a ±0.5 °C hysteresis band: the heater
  turned off at about setpoint + 0.45 and back on at about setpoint − 0.55.
- The heat mat keeps pushing heat for 3–4 minutes after it switches off,
  which stretched that band to a **±1.1 °C limit cycle** with a period of
  about 21 minutes. At a 34 °C setpoint it ran 32.6–34.8, averaging about
  0.3 °C low.
- The derivative term did almost nothing, because the change in error per
  5 s step is about 0.03 °C.
- The Kasa plug only changed state when telemetry arrived (every 30 s), so
  any switching decision could be up to 30 s late.

The roughly 40 °C plateaus on the long-range chart are holds at a higher
setpoint. They are not overshoot: the heat mat alone tops out at about 40 °C.

## Plant model

A two-state model was fitted to logged telemetry with least squares on
2026-11-10, covering 1.7 h of cycling at a 34 °C setpoint. The RMS error
was 0.2 °C, against 0.0625 °C sensor resolution.

```
dTm/dt = k1·u − k2·(Tm − T)        Tm = heat mat, u = heater on/off
dT/dt  = k3·(Tm − T) − k4·(T − Ta) T  = probe, Ta = ambient
```

| | fit (cycling) | fit (incl. warm-up) |
|---|---|---|
| time constants | 5.9 min, 18.8 min | 4.6 min, 15.7 min |
| delay | 15 s | 15 s |
| 100 % heater steady state | 40.6 °C | 38.4 °C |
| duty to hold 34 °C | ≈ 0.63 | ≈ 0.65 |

## Current controller

`firmware/main/control/pid.c`, with tuning `#define`s at the top of
`app_main.c`:

- **PID in physical units:** `Kp = 0.15 /°C`, `Ti = 1200 s`, `Td = 60 s`,
  with an explicit `dt`.
- **Derivative on the measurement:** taken from a 60 s low-pass filtered
  measurement, so setpoint changes don't kick it and sensor steps don't
  make it spike.
- **Conditional integration:** the integrator freezes while the output is
  saturated in the direction it would push. Warm-up therefore doesn't wind
  it up, and the integral settles to the holding duty.
- **Time-proportioned output:** each 420 s window the heater is on for
  `duty × 420 s`. Pulses under 60 s are skipped (or the whole window is
  filled), and a pulse ends early if demand drops.
- **Immediate telemetry on relay change:** the plug follows within about a
  second instead of up to 30 s later. This adds roughly 2 extra telemetry
  rows per window.
- **Integral persistence:** the integral is saved to NVS (`pid_i`, at most
  once every 10 minutes) and restored on boot if the first reading is
  within 1 °C of the setpoint. Without this, a reboot or OTA during a hold
  sags about 3 °C for an hour. A cold warm-up starts from zero.
- **Probe fault:** both relays go off and the output window and derivative
  filter restart. The integral is kept.

### Simulated results

These are worst cases over both model fits, with heater power ×0.7–1.3,
+60 s extra delay, and a 2 °C drop in ambient temperature:

| | swing at hold (p-p) | warm-up overshoot | plug switches/h |
|---|---|---|---|
| old controller | 2.7 °C | 1.6 °C | 3.4 |
| window 300 s | 0.3 °C | 0.14 °C | 11.8 |
| **window 420 s (default)** | **0.45 °C** | **0.2 °C** | **8.3** |
| window 600 s | 0.85 °C | 0.4 °C | 5.9 |

The trade-off is the last degree of warm-up. With a low Kp and an
integrator that has to build up the holding duty, the median time to come
within ±0.3 °C is about 36 minutes from cold. Raising Kp to 0.3 speeds this
up slightly, at the cost of a bit more overshoot.

To retune: change `TPO_WINDOW_S` for the precision versus plug-wear
trade-off, or `PID_KP` / `PID_TI_S` / `PID_TD_S`. Then watch the serial log
line `PID: temp=… out=… (I=…)`. The `I` value is the learned holding duty.
