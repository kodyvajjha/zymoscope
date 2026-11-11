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
        # Every target change, so batch stats know the target at any time.
        await db.execute(
            """
            CREATE TABLE IF NOT EXISTS setpoint_log (
                id        INTEGER PRIMARY KEY AUTOINCREMENT,
                device_id TEXT NOT NULL,
                ts        REAL NOT NULL,
                setpoint  REAL NOT NULL
            )
            """
        )
        await db.execute(
            "CREATE INDEX IF NOT EXISTS idx_setpoint_log ON setpoint_log(device_id, ts)"
        )
        # Seed the log from the current setpoint of devices that have none.
        await db.execute(
            """
            INSERT INTO setpoint_log (device_id, ts, setpoint)
            SELECT device_id, CAST(strftime('%s', updated_at) AS REAL), setpoint
            FROM device_state
            WHERE setpoint IS NOT NULL
              AND device_id NOT IN (SELECT device_id FROM setpoint_log)
            """
        )
        # Batch columns added after the original schema.
        cur = await db.execute("PRAGMA table_info(batches)")
        cols = {r[1] for r in await cur.fetchall()}
        if "target" not in cols:   # manual target, for batches before setpoint_log
            await db.execute("ALTER TABLE batches ADD COLUMN target REAL")
        if "stats" not in cols:    # JSON from stats.compute(), saved at end
            await db.execute("ALTER TABLE batches ADD COLUMN stats TEXT")
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


async def set_setpoint(device_id: str, setpoint: float, log: bool = True) -> None:
    """Upsert the target temperature for a device, shared across clients.

    log=False leaves setpoint_log alone: devices whose telemetry reports
    the setpoint they actually run are logged from telemetry instead
    (see log_setpoint), so the history records what the device did, when
    it did it."""
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
    if log:
        await log_setpoint(device_id, setpoint)


async def log_setpoint(device_id: str, setpoint: float, ts: float | None = None) -> bool:
    """Append to setpoint_log if the target differs from the last entry.
    Returns True if a row was added."""
    setpoint = round(float(setpoint), 2)
    async with _get_conn() as db:
        cur = await db.execute(
            "SELECT setpoint FROM setpoint_log WHERE device_id = ? ORDER BY ts DESC LIMIT 1",
            (device_id,),
        )
        last = await cur.fetchone()
        if last is not None and abs(last[0] - setpoint) < 0.005:
            return False
        await db.execute(
            "INSERT INTO setpoint_log (device_id, ts, setpoint) VALUES (?, ?, ?)",
            (device_id, ts if ts is not None else time.time(), setpoint),
        )
        await db.commit()
        return True


async def get_setpoint_history(device_id: str, hours: float = 0) -> list[list[float]]:
    """Target changes as [ts, setpoint] pairs, oldest first. With hours > 0,
    only changes in the last `hours`, plus the one in force at its start."""
    async with _get_conn() as db:
        if hours and hours > 0:
            t0 = time.time() - hours * 3600
            cur = await db.execute(
                """
                SELECT ts, setpoint FROM setpoint_log
                WHERE device_id = :d
                  AND ts >= COALESCE((SELECT MAX(ts) FROM setpoint_log
                                      WHERE device_id = :d AND ts <= :t0), :t0)
                ORDER BY ts
                """,
                {"d": device_id, "t0": t0},
            )
        else:
            cur = await db.execute(
                "SELECT ts, setpoint FROM setpoint_log WHERE device_id = ? ORDER BY ts",
                (device_id,),
            )
        return [[r[0], r[1]] for r in await cur.fetchall()]


async def get_active_batch(device_id: str) -> dict[str, Any] | None:
    """The running batch on a device (newest if several), with its age."""
    async with _get_conn() as db:
        cur = await db.execute(
            """
            SELECT id, name,
                   CAST(strftime('%s', started_at) AS REAL) AS started_ts
            FROM batches
            WHERE device_id = ? AND ended_at IS NULL
            ORDER BY started_at DESC LIMIT 1
            """,
            (device_id,),
        )
        row = await cur.fetchone()
        return dict(row) if row else None


# ---- chart series / export ---------------------------------------------------

# First DS18B20 reading as a number. temp_c is stored as a JSON array
# ("[21.5]", "[]" when the probe failed) or, in very old rows, a bare number.
_TEMP_SQL = """
    CASE
        WHEN json_valid(temp_c) AND json_type(temp_c) = 'array'
            THEN json_extract(temp_c, '$[0]')
        ELSE CAST(temp_c AS REAL)
    END
"""


# Long ranges aggregate every stored row (~2-5 s for 100k+ rows on a Pi 3),
# so their results are cached briefly. The page appends live readings on
# top, and at these zoom levels a few stale minutes are invisible.
_SERIES_CACHE_TTL = 300.0
_SERIES_CACHE_MIN_HOURS = 168
_series_cache: dict[tuple[str, float, int], tuple[float, dict[str, Any]]] = {}


async def get_series(
    device_id: str, hours: float = 24, max_points: int = 1500
) -> dict[str, Any]:
    cacheable = not hours or hours <= 0 or hours >= _SERIES_CACHE_MIN_HOURS
    key = (device_id, float(hours or 0), int(max_points))
    if cacheable:
        hit = _series_cache.get(key)
        if hit and hit[0] > time.monotonic():
            return hit[1]
    result = await _compute_series(device_id, hours, max_points)
    if cacheable:
        _series_cache[key] = (time.monotonic() + _SERIES_CACHE_TTL, result)
    return result


async def _compute_series(
    device_id: str, hours: float, max_points: int
) -> dict[str, Any]:
    """Return a time-bucketed series for charting.

    Aggregates in SQL so the browser receives at most ~max_points rows no
    matter how long the range is (a raw "All" fetch can be hundreds of
    thousands of rows). Each point is
    [t, temp_avg, temp_min, temp_max, heat_duty, humidity, pressure, n]
    where heat_duty is the fraction of samples with the heater relay on.
    Implausible probe readings (glitches from before the firmware's CRC /
    power-on checks) are dropped from the chart; the CSV export stays raw.
    """
    max_points = max(50, min(int(max_points), 5000))
    since = time.time() - hours * 3600 if hours and hours > 0 else 0.0
    args: dict[str, Any] = {"d": device_id, "since": since}
    where = "device_id = :d AND timestamp >= :since"

    async with _get_conn() as db:
        cur = await db.execute(
            f"SELECT MIN(timestamp), MAX(timestamp), COUNT(*) FROM telemetry WHERE {where}",
            args,
        )
        t0, t1, count = await cur.fetchone()
        if not count:
            return {"bucket_s": 0, "count": 0, "points": []}

        # Never bucket finer than the 30 s publish interval.
        span = max(1.0, t1 - t0)
        bucket = max(30, int(span / max_points) + 1)

        cur = await db.execute(
            f"""
            SELECT CAST(timestamp / :b AS INTEGER) * :b AS t,
                   AVG(temp), MIN(temp), MAX(temp),
                   AVG(COALESCE(relay1, 0)),
                   AVG(humidity), AVG(pressure),
                   COUNT(*)
            FROM (
                SELECT timestamp, relay1, humidity, pressure,
                       CASE WHEN t BETWEEN -10 AND 100 AND t <> 85.0 THEN t END AS temp
                FROM (
                    SELECT timestamp, relay1, humidity, pressure, {_TEMP_SQL} AS t
                    FROM telemetry WHERE {where}
                )
            )
            GROUP BY 1
            ORDER BY 1
            """,
            {**args, "b": bucket},
        )
        rows = await cur.fetchall()

    def r(v: Any, nd: int) -> Any:
        return None if v is None else round(v, nd)

    points = [
        [int(t) + bucket // 2 if bucket > 30 else int(t),
         r(ta, 2), r(tmin, 2), r(tmax, 2), r(duty, 3), r(hum, 1), r(pres, 1), n]
        for (t, ta, tmin, tmax, duty, hum, pres, n) in rows
    ]
    return {"bucket_s": bucket, "count": count, "points": points}


async def iter_telemetry_csv(device_id: str, hours: float = 0):
    """Yield CSV text (header first) of raw telemetry rows, oldest first."""
    from datetime import datetime, timezone

    yield "time_utc,unix,temp_c,humidity,pressure_hpa,relay1_heat,relay2_cool\n"
    async with _get_conn() as db:
        since = time.time() - hours * 3600 if hours and hours > 0 else 0.0
        cur = await db.execute(
            f"""
            SELECT timestamp, {_TEMP_SQL} AS temp, humidity, pressure, relay1, relay2
            FROM telemetry WHERE device_id = ? AND timestamp >= ?
            ORDER BY timestamp ASC
            """,
            (device_id, since),
        )
        while True:
            rows = await cur.fetchmany(2000)
            if not rows:
                break
            out = []
            for ts, temp, hum, pres, r1, r2 in rows:
                iso = datetime.fromtimestamp(ts, tz=timezone.utc).strftime(
                    "%Y-%m-%dT%H:%M:%SZ")
                cells = [iso, f"{ts:.0f}", temp, hum, pres, r1, r2]
                out.append(",".join("" if c is None else str(c) for c in cells))
            yield "\n".join(out) + "\n"


async def set_device_name(device_id: str, name: str) -> bool:
    """Give a device a friendly display name. Returns False if unknown."""
    async with _get_conn() as db:
        cur = await db.execute(
            "UPDATE devices SET name = ? WHERE device_id = ?", (name, device_id)
        )
        await db.commit()
        return cur.rowcount > 0


# ---- batch temperature-control stats ----------------------------------------

async def get_batch_stats_inputs(batch_id: int) -> dict[str, Any] | None:
    """Everything stats.compute() needs for a batch: its window, telemetry
    (ts, temp, relay1) and the target timeline (manual target if set,
    otherwise the setpoint log)."""
    async with _get_conn() as db:
        cur = await db.execute(
            """
            SELECT id, device_id, target,
                   CAST(strftime('%s', started_at) AS REAL) AS t0,
                   CAST(strftime('%s', ended_at) AS REAL) AS t1
            FROM batches WHERE id = ?
            """,
            (batch_id,),
        )
        b = await cur.fetchone()
        if not b:
            return None
        t0, t1 = b["t0"], b["t1"] or time.time()
        cur = await db.execute(
            f"""
            SELECT timestamp,
                   CASE WHEN t BETWEEN -10 AND 100 AND t <> 85.0 THEN t END,
                   relay1
            FROM (SELECT timestamp, relay1, {_TEMP_SQL} AS t FROM telemetry
                  WHERE device_id = ? AND timestamp BETWEEN ? AND ?)
            ORDER BY timestamp
            """,
            (b["device_id"], t0, t1),
        )
        rows = [tuple(r) for r in await cur.fetchall()]
        if b["target"] is not None:
            targets, source = [(t0, float(b["target"]))], "manual"
        else:
            cur = await db.execute(
                """
                SELECT ts, setpoint FROM setpoint_log
                WHERE device_id = :d AND ts <= :t1
                  AND ts >= COALESCE((SELECT MAX(ts) FROM setpoint_log
                                      WHERE device_id = :d AND ts <= :t0), :t0)
                ORDER BY ts
                """,
                {"d": b["device_id"], "t0": t0, "t1": t1},
            )
            targets, source = [tuple(r) for r in await cur.fetchall()], "setpoint log"
        return {"t0": t0, "t1": t1, "ended": b["t1"] is not None,
                "rows": rows, "targets": targets, "target_source": source,
                "manual_target": b["target"]}


async def suggest_batch_target(batch_id: int) -> float | None:
    """Median probe temperature over the second half of a batch, rounded to
    0.5 °C: a starting guess for the target of an old batch."""
    inp = await get_batch_stats_inputs(batch_id)
    if not inp or not inp["rows"]:
        return None
    mid = (inp["t0"] + inp["t1"]) / 2
    temps = sorted(T for t, T, _ in inp["rows"] if T is not None and t >= mid)
    if not temps:
        return None
    return round(temps[len(temps) // 2] * 2) / 2


async def set_batch_target(batch_id: int, target: float | None) -> None:
    async with _get_conn() as db:
        await db.execute("UPDATE batches SET target = ? WHERE id = ?", (target, batch_id))
        await db.commit()


async def save_batch_stats(batch_id: int, stats_json: str | None) -> None:
    async with _get_conn() as db:
        await db.execute("UPDATE batches SET stats = ? WHERE id = ?", (stats_json, batch_id))
        await db.commit()
