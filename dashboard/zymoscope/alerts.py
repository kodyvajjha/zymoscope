"""Phone alerts via a self-hosted ntfy server (https://ntfy.sh).

One notification when something starts and one when it clears — never a
stream. Per device:

  offline         no telemetry for ALERT_OFFLINE_SEC (the plug watchdog
                  switches the heater off at about the same time)
  back online     after an offline alert
  probe fault     the DS18B20 gives no reading twice in a row (~1 min);
                  the firmware holds the heater off meanwhile
  probe ok        after a probe-fault alert
  target reached  the first time the temperature reaches a new target
                  (after a target change, or a fresh start away from it)
  out of range    once the target was reached, more than ALERT_RANGE_C
                  away from it for ALERT_RANGE_MIN minutes
  back in range   after an out-of-range alert, within 1 °C again

Disabled unless NTFY_URL and NTFY_TOPIC are set. Times are passed in so
the logic can be tested without waiting.
"""

from __future__ import annotations

import asyncio
import json
import logging
import time
import urllib.request
from dataclasses import dataclass
from typing import Any, Awaitable, Callable

from .config import settings

log = logging.getLogger(__name__)

PROBE_FAULT_READINGS = 2        # consecutive empty readings before alerting
PROBE_OK_READINGS = 3           # consecutive good readings before "probe ok"
REACHED_C = 0.3                 # "reached" = within this of the target (or crossed it)
BACK_IN_RANGE_C = 1.0


def parse_temp(temp_c: Any) -> float | None:
    """First probe reading from telemetry, None if missing or implausible
    (85.0 is the DS18B20 power-on value)."""
    if isinstance(temp_c, list):
        temp_c = temp_c[0] if temp_c else None
    try:
        t = float(temp_c)
    except (TypeError, ValueError):
        return None
    if not -10 <= t <= 100 or t == 85.0:
        return None
    return t


def fmt_dur(sec: float) -> str:
    if sec < 90:
        return f"{int(round(sec))} s"
    m = int(round(sec / 60))
    if m < 60:
        return f"{m} min"
    h, m = divmod(m, 60)
    return f"{h} h {m:02d} min" if h < 48 else f"{h // 24} d {h % 24} h"


@dataclass
class _Dev:
    last_seen: float
    offline_alerted: bool = False
    bad_readings: int = 0
    good_readings: int = 0
    probe_alerted: bool = False
    target: float | None = None
    target_since: float = 0.0
    reached: bool = True
    approach: int = 1              # +1 heating up to the target, -1 cooling
    out_since: float | None = None
    range_alerted: bool = False


Sender = Callable[[dict[str, Any]], Awaitable[None]]


class AlertEngine:
    def __init__(self, send: Sender, offline_sec: float, range_c: float,
                 range_min: float, click_url: str = "") -> None:
        self.send = send
        self.offline_sec = offline_sec
        self.range_c = range_c
        self.range_sec = range_min * 60
        self.click_url = click_url
        self.devs: dict[str, _Dev] = {}
        self.names: dict[str, str] = {}

    async def _notify(self, device_id: str, title: str, message: str,
                      priority: int = 3, tags: list[str] | None = None) -> None:
        name = self.names.get(device_id) or device_id
        msg = {"title": f"{name}: {title}", "message": message,
               "priority": priority, "tags": tags or []}
        if self.click_url:
            msg["click"] = self.click_url
        try:
            await self.send(msg)
        except Exception:
            log.exception("ntfy send failed (%s)", title)

    def seed(self, device_id: str, last_seen: float, now: float) -> None:
        """Known device at startup. One that is already silent is treated
        as already reported, so restarts don't re-send "offline" (but it
        still gets "back online")."""
        d = _Dev(last_seen=last_seen)
        d.offline_alerted = now - last_seen > self.offline_sec
        self.devs[device_id] = d

    async def on_telemetry(self, device_id: str, payload: dict[str, Any],
                           now: float, fallback_target: float | None = None) -> None:
        d = self.devs.get(device_id)
        if d is None:
            d = self.devs[device_id] = _Dev(last_seen=now)
        if d.offline_alerted:
            d.offline_alerted = False
            await self._notify(device_id, "back online",
                               f"Reporting again after {fmt_dur(now - d.last_seen)}.",
                               tags=["white_check_mark"])
        d.last_seen = now

        temp = parse_temp(payload.get("temp_c"))
        target = payload.get("setpoint", fallback_target)
        try:
            target = float(target) if target is not None else None
        except (TypeError, ValueError):
            target = None

        # --- probe
        if temp is None:
            d.good_readings = 0
            d.bad_readings += 1
            if d.bad_readings >= PROBE_FAULT_READINGS and not d.probe_alerted:
                d.probe_alerted = True
                await self._notify(device_id, "probe fault",
                                   "The temperature probe isn't reading. The heater is held "
                                   "off until it recovers — check the probe cable.",
                                   priority=4, tags=["warning"])
            d.out_since = None      # no reading: can't judge the range
            return
        d.bad_readings = 0
        d.good_readings += 1
        if d.probe_alerted and d.good_readings >= PROBE_OK_READINGS:
            d.probe_alerted = False
            await self._notify(device_id, "probe OK",
                               f"Reading again: {temp:.1f} °C.", tags=["white_check_mark"])

        if target is None:
            return

        # --- target tracking
        if d.target is None or abs(target - d.target) >= 0.05:
            first = d.target is None
            d.target, d.target_since = target, now
            d.out_since, d.range_alerted = None, False
            # A fresh start right at the target needs no "reached" alert.
            d.reached = first and abs(temp - target) <= 1.0
            d.approach = 1 if temp < target else -1
        if not d.reached:
            if abs(temp - target) <= REACHED_C or d.approach * (temp - target) >= 0:
                d.reached = True
                await self._notify(device_id, f"reached {target:.1f} °C",
                                   f"Now {temp:.1f} °C, {fmt_dur(now - d.target_since)} "
                                   f"after the target was set.", tags=["dart"])
            return

        # --- out of range (only once the target has been reached)
        off = temp - target
        if abs(off) > self.range_c:
            if d.out_since is None:
                d.out_since = now
            if not d.range_alerted and now - d.out_since >= self.range_sec:
                d.range_alerted = True
                word = "above" if off > 0 else "below"
                await self._notify(device_id, f"{abs(off):.1f} °C {word} target",
                                   f"{temp:.1f} °C against a target of {target:.1f} °C "
                                   f"for {fmt_dur(now - d.out_since)}.",
                                   priority=4, tags=["fire" if off > 0 else "snowflake"])
        else:
            d.out_since = None
            if d.range_alerted and abs(off) <= BACK_IN_RANGE_C:
                d.range_alerted = False
                await self._notify(device_id, "back in range",
                                   f"{temp:.1f} °C, target {target:.1f} °C.",
                                   tags=["white_check_mark"])

    async def check_offline(self, now: float) -> None:
        for device_id, d in self.devs.items():
            if not d.offline_alerted and now - d.last_seen > self.offline_sec:
                d.offline_alerted = True
                when = time.strftime("%H:%M", time.localtime(d.last_seen))
                await self._notify(device_id, "offline",
                                   f"No data since {when}. The heater plug is switched off "
                                   f"while it's silent.", priority=4, tags=["rotating_light"])


# ---- ntfy transport + module-level engine ------------------------------------

def _post(msg: dict[str, Any]) -> None:
    body = dict(msg, topic=settings.NTFY_TOPIC)
    req = urllib.request.Request(
        settings.NTFY_URL.rstrip("/"), data=json.dumps(body).encode(),
        headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=10) as r:
        r.read()


async def ntfy_send(msg: dict[str, Any]) -> None:
    await asyncio.to_thread(_post, msg)


def enabled() -> bool:
    return bool(settings.NTFY_URL and settings.NTFY_TOPIC)


engine = AlertEngine(ntfy_send, settings.ALERT_OFFLINE_SEC, settings.ALERT_RANGE_C,
                     settings.ALERT_RANGE_MIN, settings.ALERT_CLICK_URL)
