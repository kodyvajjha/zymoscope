"""FastAPI server for the Zymoscope dashboard."""

from __future__ import annotations

import asyncio
import json
import logging
import math
import time
from contextlib import asynccontextmanager
from pathlib import Path
from typing import Any

from fastapi import FastAPI, WebSocket, WebSocketDisconnect, Query
from fastapi.responses import HTMLResponse, Response, StreamingResponse
from fastapi.staticfiles import StaticFiles
from fastapi.templating import Jinja2Templates
from starlette.requests import Request

from . import alerts, db, mqtt_sub, smart_plug, stats
from .config import settings

log = logging.getLogger(__name__)

BASE_DIR = Path(__file__).resolve().parent.parent
TEMPLATES_DIR = BASE_DIR / "templates"
STATIC_DIR = BASE_DIR / "static"

templates = Jinja2Templates(directory=str(TEMPLATES_DIR))

# Connected WebSocket clients.
_ws_clients: set[WebSocket] = set()

# Target range the firmware accepts (SETPOINT_MIN_C/MAX_C in mqtt_client.c).
SP_MIN, SP_MAX = 0.0, 60.0
# After a target change from the dashboard, ignore the device's reported
# target this long before trusting it over ours (telemetry already in
# flight still carries the old value).
SP_CMD_GRACE_S = 90
_last_sp_cmd: dict[str, float] = {}


def _broadcast(device_id: str, payload: dict[str, Any]) -> None:
    """Called from the MQTT thread to fan-out to WebSocket clients and smart plugs."""
    loop = asyncio.get_event_loop() if False else mqtt_sub._loop
    if loop is None:
        return
    msg = json.dumps({"device_id": device_id, **payload})
    for ws in list(_ws_clients):
        asyncio.run_coroutine_threadsafe(_safe_send(ws, msg), loop)
    # Forward relay state changes to Kasa smart plugs.
    asyncio.run_coroutine_threadsafe(smart_plug.on_telemetry(device_id, payload), loop)
    if payload.get("setpoint") is not None:
        asyncio.run_coroutine_threadsafe(
            _on_reported_setpoint(device_id, payload["setpoint"],
                                  payload.get("timestamp")), loop)
    if alerts.enabled():
        asyncio.run_coroutine_threadsafe(_alert_telemetry(device_id, payload), loop)


async def _alert_telemetry(device_id: str, payload: dict[str, Any]) -> None:
    try:
        fallback = None
        if payload.get("setpoint") is None:     # older firmware: use the UI target
            state = await db.get_device_state(device_id)
            fallback = state.get("setpoint") if state else None
        await alerts.engine.on_telemetry(device_id, payload,
                                         payload.get("timestamp") or time.time(), fallback)
    except Exception:
        log.exception("Alert processing failed")


async def _alerts_loop() -> None:
    """Seed alert state from the DB, then check for silent devices."""
    now = time.time()
    for dev in await db.get_devices():
        alerts.engine.names[dev["device_id"]] = dev.get("name") or dev["device_id"]
        last = await db.get_last_telemetry_ts(dev["device_id"])
        if last:
            alerts.engine.seed(dev["device_id"], last, now)
    while True:
        try:
            for dev in await db.get_devices():      # pick up renames
                alerts.engine.names[dev["device_id"]] = dev.get("name") or dev["device_id"]
            await alerts.engine.check_offline(time.time())
        except Exception:
            log.exception("Alert check failed")
        await asyncio.sleep(30)


def _device_reports_setpoint(device_id: str) -> bool:
    """Newer firmware includes its active target in telemetry."""
    return mqtt_sub.latest.get(device_id, {}).get("setpoint") is not None


async def _on_reported_setpoint(device_id: str, value: Any,
                                ts: float | None = None) -> None:
    """Log the target the device is actually running (batch history), and
    correct the shared UI value if it differs (e.g. a command it missed
    while offline)."""
    try:
        sp = float(value)
    except (TypeError, ValueError):
        return
    if not math.isfinite(sp):
        return
    try:
        # Stamp with the telemetry row's time (just before it, so that row
        # already counts toward the new target).
        await db.log_setpoint(device_id, sp, ts - 0.001 if ts else None)
        if time.time() - _last_sp_cmd.get(device_id, 0) < SP_CMD_GRACE_S:
            return
        state = await db.get_device_state(device_id)
        if not state or state.get("setpoint") is None or abs(state["setpoint"] - sp) >= 0.05:
            await db.set_setpoint(device_id, sp, log=False)
            await _broadcast_state(device_id, {"setpoint": sp})
    except Exception:
        log.exception("Recording reported setpoint failed")


def _on_status(device_id: str, payload: dict[str, Any]) -> None:
    """MQTT thread: device status events. When a device (re)connects,
    send it the batch to show on its screen."""
    loop = mqtt_sub._loop
    if loop is not None and payload.get("event") == "online":
        asyncio.run_coroutine_threadsafe(_push_batch_to_device(device_id), loop)


def _oled_name(name: str) -> str:
    """Batch name the OLED can show: printable ASCII, no quotes or
    backslashes (the firmware's JSON parser has no escapes), 23 chars."""
    clean = "".join(c for c in name.strip() if " " <= c <= "~" and c not in '"\\')
    return " ".join(clean.split())[:23].strip()


async def _push_batch_to_device(device_id: str | None) -> None:
    if not device_id:
        return
    try:
        b = await db.get_active_batch(device_id)
        if b:
            payload = {"batch_name": _oled_name(b["name"]) or f"Batch {b['id']}",
                       "batch_elapsed": max(0, int(time.time() - (b["started_ts"] or time.time())))}
        else:
            payload = {"batch_name": ""}
        mqtt_sub.publish_command(device_id, payload)
    except RuntimeError:
        pass   # broker down; the device gets it on its next "online"
    except Exception:
        log.exception("Sending batch info to %s failed", device_id)


def _parse_setpoint(value: Any) -> float | None:
    try:
        sp = float(value)
    except (TypeError, ValueError):
        return None
    return sp if math.isfinite(sp) and SP_MIN <= sp <= SP_MAX else None


async def _store_setpoint_from_ui(device_id: str, sp: float) -> None:
    """Shared UI value. The history (setpoint_log) is written from the
    device's telemetry when it reports its target, else here."""
    _last_sp_cmd[device_id] = time.time()
    await db.set_setpoint(device_id, sp, log=not _device_reports_setpoint(device_id))


async def _broadcast_state(device_id: str, state: dict[str, Any]) -> None:
    """Push a shared-state update (e.g. setpoint change) to all WS clients."""
    msg = json.dumps({"type": "state", "device_id": device_id, **state})
    for ws in list(_ws_clients):
        await _safe_send(ws, msg)


async def _safe_send(ws: WebSocket, text: str) -> None:
    try:
        await ws.send_text(text)
    except Exception:
        _ws_clients.discard(ws)


# ---- Lifespan ----------------------------------------------------------------

@asynccontextmanager
async def lifespan(app: FastAPI):
    # Startup
    await db.init_db()
    mqtt_sub.set_event_loop(asyncio.get_running_loop())
    mqtt_sub.register_callback(_broadcast)
    mqtt_sub.register_status_callback(_on_status)
    mqtt_sub.start(daemon=True)
    # Start smart plug energy polling + stale-telemetry watchdog (every 30s)
    # if plugs are configured.
    energy_task = None
    if settings.KASA_HEATER_HOST or settings.KASA_COOLER_HOST:
        started_at = time.time()

        async def _poll_energy_loop() -> None:
            while True:
                try:
                    # Count server start as "last seen" so a node that is
                    # already dead when the dashboard boots still trips it.
                    last_ts = max(
                        [started_at] + [
                            d.get("timestamp", 0) for d in mqtt_sub.latest.values()
                        ]
                    )
                    await smart_plug.watchdog(last_ts, time.time())
                    await smart_plug.poll_energy()
                except Exception:
                    log.exception("Smart plug poll/watchdog error")
                await asyncio.sleep(30)
        energy_task = asyncio.create_task(_poll_energy_loop())
        log.info("Smart plug energy polling + watchdog started")
    alerts_task = None
    if alerts.enabled():
        alerts_task = asyncio.create_task(_alerts_loop())
        log.info("ntfy alerts enabled (topic %s…)", settings.NTFY_TOPIC[:6])
    log.info("Zymoscope dashboard started")
    yield
    # Shutdown
    if energy_task:
        energy_task.cancel()
    if alerts_task:
        alerts_task.cancel()
    mqtt_sub.unregister_callback(_broadcast)
    mqtt_sub.unregister_status_callback(_on_status)


app = FastAPI(title="Zymoscope", lifespan=lifespan)
app.mount("/static", StaticFiles(directory=str(STATIC_DIR)), name="static")


# ---- Routes -----------------------------------------------------------------

@app.get("/", response_class=HTMLResponse)
async def index(request: Request):
    devices = await db.get_devices()
    return templates.TemplateResponse(request, "index.html", {
        "devices": devices,
        "latest": mqtt_sub.latest,
    })


@app.get("/api/devices")
async def api_devices():
    return await db.get_devices()


@app.post("/api/devices/{device_id}")
async def api_rename_device(device_id: str, request: Request):
    """Set a device's friendly display name: {"name": "Koji chamber"}."""
    data = await request.json()
    name = str(data.get("name", "")).strip()[:40] or device_id
    if not await db.set_device_name(device_id, name):
        return Response('{"error":"unknown device"}', status_code=404,
                        media_type="application/json")
    return {"status": "ok", "device_id": device_id, "name": name}


@app.get("/api/telemetry/{device_id}")
async def api_telemetry(device_id: str, hours: float = Query(24)):
    """Raw rows (can be very large for long ranges — prefer /api/series)."""
    return await db.get_recent_telemetry(device_id, hours)


@app.get("/api/series/{device_id}")
async def api_series(device_id: str, hours: float = Query(24),
                     points: int = Query(1500)):
    """Time-bucketed chart series; size is bounded regardless of range."""
    series = await db.get_series(device_id, hours, points)
    # Serialize directly: FastAPI's jsonable_encoder is slow on a Pi.
    return Response(json.dumps(series, separators=(",", ":")),
                    media_type="application/json")


@app.get("/api/export/{device_id}.csv")
async def api_export_csv(device_id: str, hours: float = Query(0)):
    """Download raw telemetry as CSV (hours <= 0 = everything)."""
    label = "all" if hours <= 0 else f"{hours:g}h"
    fname = f"zymoscope-{device_id}-{label}-{time.strftime('%Y%m%d')}.csv"
    return StreamingResponse(
        db.iter_telemetry_csv(device_id, hours),
        media_type="text/csv",
        headers={"Content-Disposition": f'attachment; filename="{fname}"'},
    )


@app.get("/api/telemetry/{device_id}/latest")
async def api_telemetry_latest(device_id: str):
    """Return the most recent stored row for a device (handy when the node is offline)."""
    row = await db.get_latest_telemetry(device_id)
    return row or {}


@app.get("/api/batches")
async def api_batches():
    return await db.get_batches()


@app.post("/api/batches")
async def api_create_batch(request: Request):
    data = await request.json()
    if not data.get("device_id"):
        return {"error": "device_id is required"}
    batch_id = await db.create_batch(data)
    await _push_batch_to_device(data.get("device_id"))
    return {"id": batch_id, "status": "created"}


@app.get("/api/batches/{batch_id}")
async def api_batch(batch_id: int):
    batch = await db.get_batch(batch_id)
    return batch or {}


@app.get("/api/batches/{batch_id}/telemetry")
async def api_batch_telemetry(batch_id: int):
    return await db.get_batch_telemetry(batch_id)


@app.post("/api/batches/{batch_id}/end")
async def api_end_batch(batch_id: int):
    await db.end_batch(batch_id)
    batch = await db.get_batch(batch_id)
    await _push_batch_to_device(batch["device_id"] if batch else None)
    result = None
    try:
        result = await _batch_stats(batch_id, save=True)
    except Exception:
        log.exception("Computing stats for batch %s failed", batch_id)
    return {"status": "ended", "id": batch_id, "stats": result}


async def _batch_stats(batch_id: int, save: bool = False) -> dict[str, Any] | None:
    inp = await db.get_batch_stats_inputs(batch_id)
    if inp is None:
        return None
    # A minute of CPU-bound work at most for very long batches; keep the
    # event loop (MQTT ingest, websockets) responsive.
    result = await asyncio.to_thread(
        stats.compute, inp["rows"], inp["targets"], inp["t0"], inp["t1"])
    result["target_source"] = inp["target_source"]
    result["in_progress"] = not inp["ended"]
    if not inp["targets"]:
        result["suggested_target"] = await db.suggest_batch_target(batch_id)
    if save and inp["ended"]:
        await db.save_batch_stats(batch_id, json.dumps(result, separators=(",", ":")))
    return result


@app.get("/api/batches/{batch_id}/stats")
async def api_batch_stats(batch_id: int):
    """Temperature-control quality for a batch (see zymoscope/stats.py).
    Computed fresh; ended batches also keep a saved copy for the list."""
    result = await _batch_stats(batch_id, save=True)
    if result is None:
        return Response('{"error":"unknown batch"}', status_code=404,
                        media_type="application/json")
    return result


@app.post("/api/batches/{batch_id}/target")
async def api_batch_target(batch_id: int, request: Request):
    """Set a manual target for a batch ({"target": 34.0}, or null to use
    the setpoint log). Needed for batches from before setpoints were logged."""
    data = await request.json()
    target = data.get("target")
    if target is not None:
        try:
            target = float(target)
        except (TypeError, ValueError):
            return Response('{"error":"invalid target"}', status_code=400,
                            media_type="application/json")
        if not -10 <= target <= 100:
            return Response('{"error":"target out of range"}', status_code=400,
                            media_type="application/json")
    if not await db.get_batch(batch_id):
        return Response('{"error":"unknown batch"}', status_code=404,
                        media_type="application/json")
    await db.set_batch_target(batch_id, target)
    return await _batch_stats(batch_id, save=True)


@app.get("/batches/{batch_id}", response_class=HTMLResponse)
async def batch_page(request: Request, batch_id: int):
    batch = await db.get_batch(batch_id)
    if not batch:
        return HTMLResponse("Batch not found", status_code=404)
    return templates.TemplateResponse(request, "batch.html", {"batch": batch})


@app.post("/api/cmd/{device_id}")
async def api_command(device_id: str, request: Request):
    data = await request.json()
    if not isinstance(data, dict):
        return Response('{"error":"expected a JSON object"}', status_code=400,
                        media_type="application/json")
    sp = None
    if "setpoint" in data:
        sp = _parse_setpoint(data["setpoint"])
        if sp is None:
            return Response(f'{{"error":"setpoint must be {SP_MIN:g}-{SP_MAX:g} °C"}}',
                            status_code=400, media_type="application/json")
    try:
        mqtt_sub.publish_command(device_id, data)
    except RuntimeError as exc:
        return {"error": str(exc)}
    # Persist & sync setpoint across dashboard clients if present in the cmd.
    if sp is not None:
        await _store_setpoint_from_ui(device_id, sp)
        await _broadcast_state(device_id, {"setpoint": sp})
    return {"status": "sent", "device_id": device_id}


@app.get("/api/setpoints/{device_id}")
async def api_setpoint_history(device_id: str, hours: float = Query(0)):
    """Target changes [[unix_ts, setpoint], ...] for the chart's target line
    (the first entry is the target in force at the start of the range)."""
    return await db.get_setpoint_history(device_id, hours)


@app.post("/api/alerts/test")
async def api_alerts_test():
    """Send a test notification to the configured ntfy topic."""
    if not alerts.enabled():
        return Response('{"error":"alerts are off (set NTFY_URL and NTFY_TOPIC)"}',
                        status_code=400, media_type="application/json")
    try:
        await alerts.ntfy_send({"title": "Zymoscope test", "message":
                                "Alerts are working. You'll hear from me when a device goes "
                                "offline, the probe fails, a target is reached, or the "
                                "temperature drifts out of range.", "tags": ["test_tube"],
                                **({"click": settings.ALERT_CLICK_URL} if settings.ALERT_CLICK_URL else {})})
    except Exception as exc:
        return Response(json.dumps({"error": f"ntfy: {exc}"}), status_code=502,
                        media_type="application/json")
    return {"status": "sent"}


@app.get("/api/state")
async def api_all_state():
    """Return shared dashboard state (setpoints, etc.) for every device."""
    return await db.get_all_device_state()


@app.get("/api/state/{device_id}")
async def api_state(device_id: str):
    """Return shared dashboard state for a single device."""
    state = await db.get_device_state(device_id)
    return state or {"device_id": device_id, "setpoint": None}


@app.post("/api/setpoint/{device_id}")
async def api_set_setpoint(device_id: str, request: Request):
    """Persist a setpoint and publish it via MQTT so every client stays in sync."""
    data = await request.json()
    sp = _parse_setpoint(data.get("setpoint")) if isinstance(data, dict) else None
    if sp is None:
        return Response(f'{{"error":"setpoint must be {SP_MIN:g}-{SP_MAX:g} °C"}}',
                        status_code=400, media_type="application/json")
    await _store_setpoint_from_ui(device_id, sp)
    try:
        mqtt_sub.publish_command(device_id, {"setpoint": sp})
    except RuntimeError as exc:
        # Even if MQTT is down, we persisted the UI value so other clients sync.
        await _broadcast_state(device_id, {"setpoint": sp})
        return {"status": "stored", "mqtt_error": str(exc), "setpoint": sp}
    await _broadcast_state(device_id, {"setpoint": sp})
    return {"status": "sent", "device_id": device_id, "setpoint": sp}


@app.get("/api/plugs")
async def api_plugs():
    """Return current smart plug status and energy readings.

    Always emits one entry per configured slot, with an `error` field when
    the plug is unreachable, so the UI can distinguish "nothing configured"
    from "configured but offline".
    """
    plugs = []
    slots = [
        (settings.KASA_HEATER_HOST, settings.KASA_HEATER_ALIAS),
        (settings.KASA_COOLER_HOST, settings.KASA_COOLER_ALIAS),
    ]
    for host, alias in slots:
        if not host:
            continue
        status = await smart_plug.get_status(host)
        if status:
            plugs.append(status)
        else:
            plugs.append({
                "host": host,
                "alias": alias,
                "error": "unreachable",
            })
    return plugs


@app.post("/api/plugs/{host}/on")
async def api_plug_on(host: str):
    ok = await smart_plug.turn_on(host)
    return {"status": "ok" if ok else "error", "host": host}


@app.post("/api/plugs/{host}/off")
async def api_plug_off(host: str):
    ok = await smart_plug.turn_off(host)
    return {"status": "ok" if ok else "error", "host": host}


@app.websocket("/ws")
async def websocket_endpoint(ws: WebSocket):
    await ws.accept()
    _ws_clients.add(ws)
    try:
        # Send current latest readings + shared state immediately.
        state = await db.get_all_device_state()
        await ws.send_text(json.dumps({
            "type": "snapshot",
            "devices": mqtt_sub.latest,
            "state": state,
        }))
        # Keep connection alive; client doesn't send much.
        while True:
            # Wait for pings / close frames.
            await ws.receive_text()
    except WebSocketDisconnect:
        pass
    finally:
        _ws_clients.discard(ws)


# ---- Main -------------------------------------------------------------------

if __name__ == "__main__":
    import uvicorn

    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    uvicorn.run(
        "zymoscope.server:app",
        host=settings.HOST,
        port=settings.PORT,
        reload=False,
    )
