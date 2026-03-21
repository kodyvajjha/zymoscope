"""SQLite database layer using aiosqlite."""

from __future__ import annotations

import time
from typing import Any

import aiosqlite

from .config import settings

_DB_PATH: str = settings.DB_PATH


async def _get_conn() -> aiosqlite.Connection:
    conn = await aiosqlite.connect(_DB_PATH)
    conn.row_factory = aiosqlite.Row
    await conn.execute("PRAGMA journal_mode=WAL")
    return conn


async def init_db() -> None:
    """Create tables if they do not exist."""
    async with await _get_conn() as db:
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
        await db.commit()


async def insert_telemetry(data: dict[str, Any]) -> None:
    """Insert a telemetry row and upsert the device record."""
    device_id = data.get("device_id", "unknown")
    async with await _get_conn() as db:
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
                data.get("temp_c"),  # stored as JSON array string
                data.get("humidity"),
                data.get("pressure"),
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
    """Return telemetry rows for a device within the last *hours* hours."""
    cutoff = time.time() - hours * 3600
    async with await _get_conn() as db:
        cursor = await db.execute(
            """
            SELECT * FROM telemetry
            WHERE device_id = ? AND timestamp >= ?
            ORDER BY timestamp ASC
            """,
            (device_id, cutoff),
        )
        rows = await cursor.fetchall()
        return [dict(r) for r in rows]


async def get_devices() -> list[dict[str, Any]]:
    """Return all registered devices."""
    async with await _get_conn() as db:
        cursor = await db.execute(
            "SELECT * FROM devices ORDER BY created_at DESC"
        )
        rows = await cursor.fetchall()
        return [dict(r) for r in rows]


async def create_batch(data: dict[str, Any]) -> int:
    """Insert a new batch and return its id."""
    async with await _get_conn() as db:
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
    async with await _get_conn() as db:
        cursor = await db.execute(
            "SELECT * FROM batches ORDER BY started_at DESC"
        )
        rows = await cursor.fetchall()
        return [dict(r) for r in rows]
