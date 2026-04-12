"""FastAPI server for the Zymoscope dashboard."""

from __future__ import annotations

import asyncio
import json
import logging
from contextlib import asynccontextmanager
from pathlib import Path
from typing import Any

from fastapi import FastAPI, WebSocket, WebSocketDisconnect, Query
from fastapi.responses import HTMLResponse
from fastapi.staticfiles import StaticFiles
from fastapi.templating import Jinja2Templates
from starlette.requests import Request

from . import db, mqtt_sub, smart_plug
from .config import settings

log = logging.getLogger(__name__)

BASE_DIR = Path(__file__).resolve().parent.parent
TEMPLATES_DIR = BASE_DIR / "templates"
STATIC_DIR = BASE_DIR / "static"

templates = Jinja2Templates(directory=str(TEMPLATES_DIR))

# Connected WebSocket clients.
_ws_clients: set[WebSocket] = set()


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
    mqtt_sub.start(daemon=True)
    # Start smart plug energy polling (every 30s) if plugs are configured.
    energy_task = None
    if settings.KASA_HEATER_HOST or settings.KASA_COOLER_HOST:
        async def _poll_energy_loop() -> None:
            while True:
                await smart_plug.poll_energy()
                await asyncio.sleep(30)
        energy_task = asyncio.create_task(_poll_energy_loop())
        log.info("Smart plug energy polling started")
    log.info("Zymoscope dashboard started")
    yield
    # Shutdown
    if energy_task:
        energy_task.cancel()
    mqtt_sub.unregister_callback(_broadcast)


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


@app.get("/api/telemetry/{device_id}")
async def api_telemetry(device_id: str, hours: float = Query(24)):
    return await db.get_recent_telemetry(device_id, hours)


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
    return {"status": "ended", "id": batch_id}


@app.get("/batches/{batch_id}", response_class=HTMLResponse)
async def batch_page(request: Request, batch_id: int):
    batch = await db.get_batch(batch_id)
    if not batch:
        return HTMLResponse("Batch not found", status_code=404)
    return templates.TemplateResponse(request, "batch.html", {"batch": batch})


@app.post("/api/cmd/{device_id}")
async def api_command(device_id: str, request: Request):
    data = await request.json()
    try:
        mqtt_sub.publish_command(device_id, data)
    except RuntimeError as exc:
        return {"error": str(exc)}
    # Persist & sync setpoint across dashboard clients if present in the cmd.
    if "setpoint" in data:
        try:
            sp = float(data["setpoint"])
            await db.set_setpoint(device_id, sp)
            await _broadcast_state(device_id, {"setpoint": sp})
        except (TypeError, ValueError):
            pass
    return {"status": "sent", "device_id": device_id}


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
    try:
        sp = float(data["setpoint"])
    except (KeyError, TypeError, ValueError):
        return {"error": "missing or invalid 'setpoint'"}
    await db.set_setpoint(device_id, sp)
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
    """Return current smart plug status and energy readings."""
    plugs = []
    for host in [settings.KASA_HEATER_HOST, settings.KASA_COOLER_HOST]:
        if host:
            status = await smart_plug.get_status(host)
            if status:
                plugs.append(status)
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
