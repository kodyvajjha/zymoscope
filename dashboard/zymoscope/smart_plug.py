"""Kasa smart plug controller for heater/cooler relay substitution.

Uses python-kasa to control TP-Link Kasa smart plugs (e.g. KP115)
over the local network. No cloud account needed.

The dashboard watches MQTT relay state changes from the ESP32's PID
controller and forwards them to the configured smart plugs.
"""

from __future__ import annotations

import asyncio
import logging
import time
from typing import Any

from .config import settings

log = logging.getLogger(__name__)

# Cached plug instances (created on first use).
_plugs: dict[str, Any] = {}

# Latest energy readings per plug alias.
energy: dict[str, dict[str, Any]] = {}


async def _get_plug(host: str) -> Any:
    """Discover and connect to a Kasa plug by IP address."""
    if host in _plugs:
        plug = _plugs[host]
        try:
            await plug.update()
            return plug
        except Exception:
            _plugs.pop(host, None)

    try:
        from kasa import Discover
        dev = await Discover.discover_single(host, timeout=5)
        await dev.update()
        _plugs[host] = dev
        log.info("Connected to smart plug at %s: %s", host, dev.alias)
        return dev
    except Exception:
        log.exception("Failed to connect to smart plug at %s", host)
        return None


async def turn_on(host: str) -> bool:
    """Turn a smart plug on. Returns True on success."""
    plug = await _get_plug(host)
    if plug is None:
        return False
    try:
        await plug.turn_on()
        await plug.update()
        log.info("Smart plug %s (%s): ON", host, plug.alias)
        return True
    except Exception:
        log.exception("Failed to turn on plug %s", host)
        return False


async def turn_off(host: str) -> bool:
    """Turn a smart plug off. Returns True on success."""
    plug = await _get_plug(host)
    if plug is None:
        return False
    try:
        await plug.turn_off()
        await plug.update()
        log.info("Smart plug %s (%s): OFF", host, plug.alias)
        return True
    except Exception:
        log.exception("Failed to turn off plug %s", host)
        return False


async def get_status(host: str) -> dict[str, Any] | None:
    """Get plug status including energy readings (for KP115)."""
    plug = await _get_plug(host)
    if plug is None:
        return None
    try:
        await plug.update()
        status: dict[str, Any] = {
            "alias": plug.alias,
            "is_on": plug.is_on,
            "host": host,
        }
        # KP115 has energy monitoring via the emeter module.
        if hasattr(plug, "emeter_realtime"):
            try:
                emeter = plug.emeter_realtime
                status["power_w"] = emeter.get("power", emeter.get("power_mw", 0) / 1000)
                status["voltage_v"] = emeter.get("voltage", emeter.get("voltage_mv", 0) / 1000)
                status["current_a"] = emeter.get("current", emeter.get("current_ma", 0) / 1000)
            except Exception:
                pass
        energy[host] = status
        return status
    except Exception:
        log.exception("Failed to get status for plug %s", host)
        return None


async def handle_relay_state(relay_num: int, state: int) -> None:
    """Called when the ESP32 publishes a relay state change via MQTT.

    Maps relay1 -> KASA_HEATER_HOST, relay2 -> KASA_COOLER_HOST.
    If the corresponding env var is not set, the relay change is ignored
    (the ESP32 controls it directly via GPIO).
    """
    if relay_num == 1:
        host = settings.KASA_HEATER_HOST
    elif relay_num == 2:
        host = settings.KASA_COOLER_HOST
    else:
        return

    if not host:
        return

    if state:
        await turn_on(host)
    else:
        await turn_off(host)


# Track previous relay states to detect changes.
_prev_relay: dict[str, tuple[int, int]] = {}


async def on_telemetry(device_id: str, payload: dict[str, Any]) -> None:
    """Process incoming telemetry and drive smart plugs on relay changes."""
    relay1 = payload.get("relay1", 0)
    relay2 = payload.get("relay2", 0)

    prev = _prev_relay.get(device_id, (None, None))

    if relay1 != prev[0]:
        await handle_relay_state(1, relay1)
    if relay2 != prev[1]:
        await handle_relay_state(2, relay2)

    _prev_relay[device_id] = (relay1, relay2)


async def poll_energy() -> None:
    """Periodically poll energy data from configured plugs."""
    hosts = [h for h in [settings.KASA_HEATER_HOST, settings.KASA_COOLER_HOST] if h]
    if not hosts:
        return
    for host in hosts:
        await get_status(host)
