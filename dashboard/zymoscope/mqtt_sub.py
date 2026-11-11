"""MQTT subscriber that runs in a background thread.

Connects to the broker, subscribes to zymoscope/+/telemetry, parses JSON
payloads, stores them via db.py, and notifies registered callbacks (used by
the WebSocket layer to push live updates).

Also subscribes to zymoscope/+/status (online/offline/OTA events). Those go
only to the status callbacks: they are not telemetry, so they must not be
stored, update `latest`, or count as a sign of life for the plug watchdog.
"""

from __future__ import annotations

import asyncio
import json
import logging
import threading
import time
from typing import Any, Callable

import paho.mqtt.client as mqtt

from .config import settings

log = logging.getLogger(__name__)

# Latest reading per device, keyed by device_id.
latest: dict[str, dict[str, Any]] = {}

# Registered callbacks: called with (device_id, payload_dict).
_callbacks: list[Callable[[str, dict[str, Any]], Any]] = []
_status_callbacks: list[Callable[[str, dict[str, Any]], Any]] = []

# Reference to the running event loop (set by the server on startup).
_loop: asyncio.AbstractEventLoop | None = None

# MQTT client instance (accessible for publishing commands).
_client: mqtt.Client | None = None


def set_event_loop(loop: asyncio.AbstractEventLoop) -> None:
    """Store a reference to the asyncio event loop for cross-thread calls."""
    global _loop
    _loop = loop


def register_callback(cb: Callable[[str, dict[str, Any]], Any]) -> None:
    _callbacks.append(cb)


def unregister_callback(cb: Callable[[str, dict[str, Any]], Any]) -> None:
    try:
        _callbacks.remove(cb)
    except ValueError:
        pass


def register_status_callback(cb: Callable[[str, dict[str, Any]], Any]) -> None:
    """cb(device_id, status_payload), called from the MQTT thread."""
    _status_callbacks.append(cb)


def unregister_status_callback(cb: Callable[[str, dict[str, Any]], Any]) -> None:
    try:
        _status_callbacks.remove(cb)
    except ValueError:
        pass


def publish_command(device_id: str, payload: dict[str, Any]) -> None:
    """Publish a command to a device via MQTT."""
    if _client is None or not _client.is_connected():
        raise RuntimeError("MQTT client is not connected")
    topic = f"zymoscope/{device_id}/cmd"
    _client.publish(topic, json.dumps(payload), qos=1)


# ---- internal paho callbacks ------------------------------------------------

def _on_connect(
    client: mqtt.Client,
    userdata: Any,
    flags: Any,
    rc: int,
    properties: Any = None,
) -> None:
    if rc == 0:
        log.info("MQTT connected to %s:%s", settings.MQTT_BROKER, settings.MQTT_PORT)
        client.subscribe("zymoscope/+/telemetry", qos=1)
        client.subscribe("zymoscope/+/status", qos=1)
    else:
        log.error("MQTT connection failed with code %s", rc)


def _on_message(
    client: mqtt.Client, userdata: Any, msg: mqtt.MQTTMessage
) -> None:
    try:
        payload: dict[str, Any] = json.loads(msg.payload)
    except (json.JSONDecodeError, UnicodeDecodeError):
        log.warning("Bad payload on %s: %r", msg.topic, msg.payload[:200])
        return

    if not isinstance(payload, dict):
        return

    # Extract device_id from topic: zymoscope/<device_id>/<kind>
    parts = msg.topic.split("/")
    if len(parts) >= 2:
        device_id = parts[1]
    else:
        device_id = payload.get("device_id", "unknown")

    if len(parts) >= 3 and parts[2] == "status":
        for cb in list(_status_callbacks):
            try:
                cb(device_id, payload)
            except Exception:
                log.exception("Status callback error")
        return
    if len(parts) >= 3 and parts[2] != "telemetry":
        return

    payload["device_id"] = device_id
    # Stamp with receive time so the snapshot sent to newly opened pages
    # carries the real age of the reading (and the DB row matches it).
    payload["timestamp"] = time.time()
    latest[device_id] = payload

    # Persist to SQLite (needs the async event loop).
    if _loop is not None:
        from . import db  # deferred to avoid circular imports at module level

        asyncio.run_coroutine_threadsafe(db.insert_telemetry(payload), _loop)

    # Notify WebSocket callbacks.
    for cb in list(_callbacks):
        try:
            cb(device_id, payload)
        except Exception:
            log.exception("Callback error")


def _on_disconnect(
    client: mqtt.Client, userdata: Any, rc: int, properties: Any = None
) -> None:
    if rc != 0:
        log.warning("MQTT unexpected disconnect (rc=%s), will auto-reconnect", rc)


# ---- public API --------------------------------------------------------------

def start(daemon: bool = True) -> threading.Thread:
    """Start the MQTT subscriber in a background thread. Returns the thread."""
    global _client

    _client = mqtt.Client(
        callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
        client_id="zymoscope-dashboard",
        protocol=mqtt.MQTTv311,
    )
    _client.on_connect = _on_connect  # type: ignore[assignment]
    _client.on_message = _on_message  # type: ignore[assignment]
    _client.on_disconnect = _on_disconnect  # type: ignore[assignment]
    _client.reconnect_delay_set(min_delay=1, max_delay=30)

    def _run() -> None:
        try:
            _client.connect(settings.MQTT_BROKER, settings.MQTT_PORT, keepalive=60)  # type: ignore[union-attr]
            _client.loop_forever()  # type: ignore[union-attr]
        except Exception:
            log.exception("MQTT thread crashed")

    thread = threading.Thread(target=_run, name="mqtt-subscriber", daemon=daemon)
    thread.start()
    log.info("MQTT subscriber thread started")
    return thread
