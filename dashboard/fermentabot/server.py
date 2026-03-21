"""FastAPI server for the FermentaBot dashboard."""

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

from . import db, mqtt_sub
from .config import settings

log = logging.getLogger(__name__)

BASE_DIR = Path(__file__).resolve().parent.parent
TEMPLATES_DIR = BASE_DIR / "templates"
STATIC_DIR = BASE_DIR / "static"

templates = Jinja2Templates(directory=str(TEMPLATES_DIR))

# Connected WebSocket clients.
_ws_clients: set[WebSocket] = set()


def _broadcast(device_id: str, payload: dict[str, Any]) -> None:
    """Called from the MQTT thread to fan-out to WebSocket clients."""
    loop = asyncio.get_event_loop() if False else mqtt_sub._loop
    if loop is None:
        return
    msg = json.dumps({"device_id": device_id, **payload})
    for ws in list(_ws_clients):
        asyncio.run_coroutine_threadsafe(_safe_send(ws, msg), loop)


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
    log.info("FermentaBot dashboard started")
    yield
    # Shutdown
    mqtt_sub.unregister_callback(_broadcast)


app = FastAPI(title="FermentaBot", lifespan=lifespan)
app.mount("/static", StaticFiles(directory=str(STATIC_DIR)), name="static")


# ---- Routes -----------------------------------------------------------------

@app.get("/", response_class=HTMLResponse)
async def index(request: Request):
    devices = await db.get_devices()
    return templates.TemplateResponse("index.html", {
        "request": request,
        "devices": devices,
        "latest": mqtt_sub.latest,
    })


@app.get("/api/devices")
async def api_devices():
    return await db.get_devices()


@app.get("/api/telemetry/{device_id}")
async def api_telemetry(device_id: str, hours: float = Query(24)):
    return await db.get_recent_telemetry(device_id, hours)


@app.get("/api/batches")
async def api_batches():
    return await db.get_batches()


@app.post("/api/batches")
async def api_create_batch(request: Request):
    data = await request.json()
    batch_id = await db.create_batch(data)
    return {"id": batch_id, "status": "created"}


@app.post("/api/cmd/{device_id}")
async def api_command(device_id: str, request: Request):
    data = await request.json()
    try:
        mqtt_sub.publish_command(device_id, data)
    except RuntimeError as exc:
        return {"error": str(exc)}
    return {"status": "sent", "device_id": device_id}


@app.websocket("/ws")
async def websocket_endpoint(ws: WebSocket):
    await ws.accept()
    _ws_clients.add(ws)
    try:
        # Send current latest readings immediately.
        if mqtt_sub.latest:
            await ws.send_text(json.dumps({
                "type": "snapshot",
                "devices": mqtt_sub.latest,
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
        "fermentabot.server:app",
        host=settings.HOST,
        port=settings.PORT,
        reload=False,
    )
