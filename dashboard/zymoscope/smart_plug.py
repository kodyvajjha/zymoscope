"""Kasa smart plug controller for heater/cooler relay substitution.

Uses python-kasa to control TP-Link Kasa smart plugs (e.g. KP115)
over the local network. No cloud account needed.

The dashboard watches MQTT relay state changes from the ESP32's PID
controller and forwards them to the configured smart plugs.

The configured *_HOST env var is the stable slot identifier exposed
through the API. Internally we cache the most recent reachable IP per
slot and, when the cached IP fails, broadcast-discover the LAN and
rebind to the device whose Kasa alias matches *_ALIAS. This lets the
heater keep working through DHCP lease shuffles and plug reboots.
"""

from __future__ import annotations

import logging
from typing import Any

from .config import settings

log = logging.getLogger(__name__)

# Connected device handles, keyed by the configured host string.
_devices: dict[str, Any] = {}

# Last reachable IP per configured host (may diverge from the configured
# host string after an alias-fallback rediscovery).
_live_ip: dict[str, str] = {}

# Latest energy readings per configured host.
energy: dict[str, dict[str, Any]] = {}


def _slot_alias(configured_host: str) -> str:
    """Return the alias configured for a given slot, or '' if none."""
    if configured_host and configured_host == settings.KASA_HEATER_HOST:
        return settings.KASA_HEATER_ALIAS
    if configured_host and configured_host == settings.KASA_COOLER_HOST:
        return settings.KASA_COOLER_ALIAS
    return ""


async def _connect_direct(ip: str) -> Any:
    """Try a direct unicast discovery against a single IP."""
    from kasa import Discover

    dev = await Discover.discover_single(ip, timeout=5)
    await dev.update()
    return dev


async def _connect_by_alias(alias: str) -> tuple[str, Any] | None:
    """Broadcast-discover the LAN, return the first device matching alias."""
    from kasa import Discover

    try:
        devices = await Discover.discover(timeout=5)
    except Exception:
        log.exception("Kasa broadcast discovery failed")
        return None

    for ip, candidate in devices.items():
        try:
            await candidate.update()
        except Exception:
            continue
        if candidate.alias == alias:
            return ip, candidate
    return None


async def _get_plug(configured_host: str) -> Any:
    """Return a connected Kasa device for the given configured host.

    Resolution order:
      1. Cached device handle (refresh via update()).
      2. Last-known live IP (or the configured host on first call).
      3. Broadcast-discover, match by configured alias, cache the new IP.
    """
    if not configured_host:
        return None

    dev = _devices.get(configured_host)
    if dev is not None:
        try:
            await dev.update()
            return dev
        except Exception:
            log.warning("Cached Kasa device for %s went stale; reconnecting", configured_host)
            _devices.pop(configured_host, None)

    candidate_ip = _live_ip.get(configured_host, configured_host)
    try:
        dev = await _connect_direct(candidate_ip)
    except Exception:
        log.info("Direct connect to Kasa at %s failed; trying alias fallback", candidate_ip)
        dev = None

    if dev is None:
        alias = _slot_alias(configured_host)
        if not alias:
            log.warning("Kasa plug %s unreachable and no alias fallback configured", configured_host)
            return None
        found = await _connect_by_alias(alias)
        if found is None:
            log.warning("No Kasa device with alias '%s' found on the LAN", alias)
            return None
        new_ip, dev = found
        if new_ip != candidate_ip:
            log.info(
                "Rebound Kasa alias '%s' to %s (was %s)",
                alias, new_ip, candidate_ip,
            )
        _live_ip[configured_host] = new_ip
    else:
        _live_ip[configured_host] = candidate_ip

    _devices[configured_host] = dev
    return dev


async def turn_on(configured_host: str) -> bool:
    """Turn a smart plug on. Returns True on success."""
    plug = await _get_plug(configured_host)
    if plug is None:
        return False
    try:
        await plug.turn_on()
        await plug.update()
        log.info("Smart plug %s (%s): ON", configured_host, plug.alias)
        return True
    except Exception:
        log.exception("Failed to turn on plug %s", configured_host)
        return False


async def turn_off(configured_host: str) -> bool:
    """Turn a smart plug off. Returns True on success."""
    plug = await _get_plug(configured_host)
    if plug is None:
        return False
    try:
        await plug.turn_off()
        await plug.update()
        log.info("Smart plug %s (%s): OFF", configured_host, plug.alias)
        return True
    except Exception:
        log.exception("Failed to turn off plug %s", configured_host)
        return False


async def get_status(configured_host: str) -> dict[str, Any] | None:
    """Get plug status including energy readings (for KP115)."""
    plug = await _get_plug(configured_host)
    if plug is None:
        return None
    try:
        await plug.update()
        live_ip = _live_ip.get(configured_host, configured_host)
        status: dict[str, Any] = {
            "alias": plug.alias,
            "is_on": plug.is_on,
            "host": configured_host,
            "live_host": live_ip,
        }
        if hasattr(plug, "emeter_realtime"):
            try:
                emeter = plug.emeter_realtime
                status["power_w"] = emeter.get("power", emeter.get("power_mw", 0) / 1000)
                status["voltage_v"] = emeter.get("voltage", emeter.get("voltage_mv", 0) / 1000)
                status["current_a"] = emeter.get("current", emeter.get("current_ma", 0) / 1000)
            except Exception:
                pass
        energy[configured_host] = status
        return status
    except Exception:
        log.exception("Failed to get status for plug %s", configured_host)
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
