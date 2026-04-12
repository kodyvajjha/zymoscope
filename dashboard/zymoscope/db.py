"""SQLite database layer using aiosqlite."""

from __future__ import annotations

import json
import time
from contextlib import asynccontextmanager
from typing import Any

import aiosqlite

from .config import settings

_DB_PATH: str = settings.DB_PATH


@asynccontextmanager
async def _get_conn():
    conn = await aiosqlite.connect(_DB_PATH)
    conn.row_factory = aiosqlite.Row
    await conn.execute("PRAGMA journal_mode=WAL")
    try:
        yield conn
    finally:
        await conn.close()


async def init_db() -> None:
    """Create tables if they do not exist."""
    async with _get_conn() as db:
        await db.execute(
            """
            CREATE TABLE IF NOT EXISTS devices (
                device_id TEXT PRIMARY KEY,
                name      TEXT,
                created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
            )
            """
        )
        await db.execute(
            """
            CREATE TABLE IF NOT EXISTS telemetry (
                id          INTEGER PRIMARY KEY AUTOINCREMENT,
                device_id   TEXT NOT NULL,
                timestamp   REAL NOT NULL,
                temp_c      TEXT,
                humidity    REAL,
                pressure    REAL,
                weight_g    REAL,
                gravity_est REAL,
                relay1      INTEGER,
                relay2      INTEGER,
                rssi        INTEGER,
                FOREIGN KEY (device_id) REFERENCES devices(device_id)
            )
            """
        )
        await db.execute(
            """
            CREATE INDEX IF NOT EXISTS idx_telemetry_device_ts
            ON telemetry(device_id, timestamp)
            """
        )
        await db.execute(
            """
            CREATE TABLE IF NOT EXISTS batches (
                id         INTEGER PRIMARY KEY AUTOINCREMENT,
                device_id  TEXT,
                name       TEXT NOT NULL,
                style      TEXT,
                og         REAL,
                started_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
                ended_at   TIMESTAMP,
                notes      TEXT,
                FOREIGN KEY (device_id) REFERENCES devices(device_id)
            )
            """
        )
        # Per-device state shared across dashboard clients (setpoint, etc).
        await db.execute(
            """
            CREATE TABLE IF NOT EXISTS device_state (
                device_id  TEXT PRIMARY KEY,
                setpoint   REAL,
                updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
                FOREIGN KEY (device_id) REFERENCES devices(device_id)
            )
            """
        )
        await db.commit()


async def insert_telemetry(data: dict[str, Any]) -> None:
    """Insert a telemetry row and upsert the device record."""
    device_id = data.get("device_id", "unknown")
    temp_c = data.get("temp_c")
    if isinstance(temp_c, (list, dict)):
        temp_c = json.dumps(temp_c)
    async with _get_conn() as db:
        await db.execute(
            """
            INSERT OR IGNORE INTO devices (device_id, name)
            VALUES (?, ?)
            """,
            (device_id, device_id),
        )
        await db.execute(
            """
            INSERT INTO telemetry
                (device_id, timestamp, temp_c, humidity, pressure,
                 weight_g, gravity_est, relay1, relay2, rssi)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            (
                device_id,
                data.get("timestamp", time.time()),
                temp_c,
                data.get("humidity"),
                data.get("pressure_hpa", data.get("pressure")),
                data.get("weight_g"),
                data.get("gravity_est"),
                data.get("relay1"),
                data.get("relay2"),
                data.get("rssi"),
            ),
        )
        await db.commit()


async def get_recent_telemetry(
    device_id: str, hours: float = 24
) -> list[dict[str, Any]]:
    """Return telemetry rows for a device within the last *hours* hours.

    Pass hours <= 0 to fetch the entire history for the device.
    """
    async with _get_conn() as db:
        if hours and hours > 0:
            cutoff = time.time() - hours * 3600
            cursor = await db.execute(
                """
                SELECT * FROM telemetry
                WHERE device_id = ? AND timestamp >= ?
                ORDER BY timestamp ASC
                """,
                (device_id, cutoff),
            )
        else:
            cursor = await db.execute(
                """
                SELECT * FROM telemetry
                WHERE device_id = ?
                ORDER BY timestamp ASC
                """,
                (device_id,),
            )
        rows = await cursor.fetchall()
        return [dict(r) for r in rows]


async def get_latest_telemetry(device_id: str) -> dict[str, Any] | None:
    """Return the most recent telemetry row for a device, or None."""
    async with _get_conn() as db:
        cursor = await db.execute(
            """
            SELECT * FROM telemetry
            WHERE device_id = ?
            ORDER BY timestamp DESC
            LIMIT 1
            """,
            (device_id,),
        )
        row = await cursor.fetchone()
        return dict(row) if row else None


async def get_devices() -> list[dict[str, Any]]:
    """Return all registered devices."""
    async with _get_conn() as db:
        cursor = await db.execute(
            "SELECT * FROM devices ORDER BY created_at DESC"
        )
        rows = await cursor.fetchall()
        return [dict(r) for r in rows]


async def create_batch(data: dict[str, Any]) -> int:
    """Insert a new batch and return its id."""
    async with _get_conn() as db:
        cursor = await db.execute(
            """
            INSERT INTO batches (device_id, name, style, og, notes)
            VALUES (?, ?, ?, ?, ?)
            """,
            (
                data.get("device_id"),
                data["name"],
                data.get("style"),
                data.get("og"),
                data.get("notes"),
            ),
        )
        await db.commit()
        return cursor.lastrowid  # type: ignore[return-value]


async def get_batches() -> list[dict[str, Any]]:
    """Return all batches, newest first."""
    async with _get_conn() as db:
        cursor = await db.execute(
            "SELECT * FROM batches ORDER BY started_at DESC"
        )
        rows = await cursor.fetchall()
        return [dict(r) for r in rows]


async def get_batch(batch_id: int) -> dict[str, Any] | None:
    """Return a single batch by id, or None."""
    async with _get_conn() as db:
        cursor = await db.execute(
            "SELECT * FROM batches WHERE id = ?", (batch_id,)
        )
        row = await cursor.fetchone()
        return dict(row) if row else None


async def end_batch(batch_id: int) -> None:
    """Set ended_at = now for a batch."""
    async with _get_conn() as db:
        await db.execute(
            "UPDATE batches SET ended_at = CURRENT_TIMESTAMP WHERE id = ?",
            (batch_id,),
        )
        await db.commit()


async def get_batch_telemetry(batch_id: int) -> list[dict[str, Any]]:
    """Return telemetry rows scoped to a batch's device and time window.

    started_at / ended_at are stored as SQLite TIMESTAMP strings (UTC).
    strftime('%s', ...) converts to unix seconds for comparison against
    telemetry.timestamp (REAL, unix seconds).
    """
    async with _get_conn() as db:
        cursor = await db.execute(
            """
            SELECT t.* FROM telemetry t
            JOIN batches b ON b.device_id = t.device_id
            WHERE b.id = ?
              AND t.timestamp >= CAST(strftime('%s', b.started_at) AS REAL)
              AND (b.ended_at IS NULL
                   OR t.timestamp <= CAST(strftime('%s', b.ended_at) AS REAL))
            ORDER BY t.timestamp ASC
            """,
            (batch_id,),
        )
        rows = await cursor.fetchall()
        return [dict(r) for r in rows]


# ---- device state (shared setpoint, etc.) -----------------------------------

async def get_device_state(device_id: str) -> dict[str, Any] | None:
    """Return the stored state row for a device, or None if not set."""
    async with _get_conn() as db:
        cursor = await db.execute(
            "SELECT * FROM device_state WHERE device_id = ?",
            (device_id,),
        )
        row = await cursor.fetchone()
        return dict(row) if row else None


async def get_all_device_state() -> dict[str, dict[str, Any]]:
    """Return the stored state row for every device, keyed by device_id."""
    async with _get_conn() as db:
        cursor = await db.execute("SELECT * FROM device_state")
        rows = await cursor.fetchall()
        return {r["device_id"]: dict(r) for r in rows}


async def set_setpoint(device_id: str, setpoint: float) -> None:
    """Upsert the target temperature for a device, shared across clients."""
    async with _get_conn() as db:
        await db.execute(
            """
            INSERT INTO device_state (device_id, setpoint, updated_at)
            VALUES (?, ?, CURRENT_TIMESTAMP)
            ON CONFLICT(device_id) DO UPDATE SET
                setpoint   = excluded.setpoint,
                updated_at = CURRENT_TIMESTAMP
            """,
            (device_id, setpoint),
        )
        await db.commit()
