"""SQLite storage for raw CAN frames. Pure stdlib so it can be tested on its own."""

import sqlite3
import time
from contextlib import closing

SCHEMA = """
CREATE TABLE IF NOT EXISTS batches (
    id          INTEGER PRIMARY KEY,
    device_id   TEXT    NOT NULL,
    boot_id     TEXT    NOT NULL,
    received_at REAL    NOT NULL,   -- server unix time
    sent_us     INTEGER NOT NULL,   -- device uptime when batch was sent
    dropped     INTEGER NOT NULL,   -- cumulative frames dropped on device
    frame_count INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS frames (
    id        INTEGER PRIMARY KEY,
    batch_id  INTEGER NOT NULL REFERENCES batches(id),
    device_id TEXT    NOT NULL,
    boot_id   TEXT    NOT NULL,
    t_us      INTEGER NOT NULL,     -- device uptime (exact relative timing)
    ts        REAL    NOT NULL,     -- estimated unix time
    bus       INTEGER NOT NULL DEFAULT 0,  -- 0 = HS-CAN, 1 = MS-CAN
    can_id    INTEGER NOT NULL,
    ext       INTEGER NOT NULL,
    dir       INTEGER NOT NULL,     -- 0 = rx, 1 = tx (our request)
    data      BLOB    NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_frames_ts       ON frames(ts);
CREATE INDEX IF NOT EXISTS idx_frames_can_ts   ON frames(can_id, ts);
CREATE INDEX IF NOT EXISTS idx_frames_boot_t   ON frames(device_id, boot_id, t_us);
"""


def connect(db_path: str) -> sqlite3.Connection:
    conn = sqlite3.connect(db_path)
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA synchronous=NORMAL")
    return conn


def init_db(db_path: str) -> None:
    with closing(connect(db_path)) as conn:
        conn.executescript(SCHEMA)
        # Upgrade databases created before the bus column existed.
        cols = {row[1] for row in conn.execute("PRAGMA table_info(frames)")}
        if "bus" not in cols:
            conn.execute("ALTER TABLE frames ADD COLUMN bus INTEGER NOT NULL DEFAULT 0")
            conn.commit()


def estimate_unix_time(received_at: float, sent_us: int, t_us: int) -> float:
    """The device has no real clock, only uptime. The batch says what its uptime
    was at send time, so each frame happened (sent_us - t_us) before we received it.
    Error is roughly the network latency, which is fine for logging."""
    return received_at - (sent_us - t_us) / 1_000_000


def store_batch(db_path: str, device_id: str, boot_id: str, sent_us: int,
                dropped: int, frames: list[dict], received_at: float | None = None) -> tuple[int, int]:
    """frames: dicts with keys t, id, ext, dir, data (hex string), optional bus.
    Returns (batch_id, stored)."""
    received_at = time.time() if received_at is None else received_at
    with closing(connect(db_path)) as conn, conn:
        cur = conn.execute(
            "INSERT INTO batches (device_id, boot_id, received_at, sent_us, dropped, frame_count) "
            "VALUES (?, ?, ?, ?, ?, ?)",
            (device_id, boot_id, received_at, sent_us, dropped, len(frames)),
        )
        batch_id = cur.lastrowid
        rows = [
            (batch_id, device_id, boot_id, f["t"],
             estimate_unix_time(received_at, sent_us, f["t"]), f.get("bus", 0),
             f["id"], f["ext"], f["dir"], bytes.fromhex(f["data"]))
            for f in frames
        ]
        conn.executemany(
            "INSERT INTO frames (batch_id, device_id, boot_id, t_us, ts, bus, can_id, ext, dir, data) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
            rows,
        )
    return batch_id, len(rows)


BUS_NAMES = {0: "hs", 1: "ms"}


def query_frames(db_path: str, since: float = 0.0, can_id: int | None = None,
                 bus: int | None = None, limit: int = 500) -> list[dict]:
    sql = ("SELECT ts, device_id, boot_id, t_us, bus, can_id, ext, dir, data "
           "FROM frames WHERE ts > ?")
    params: list = [since]
    if bus is not None:
        sql += " AND bus = ?"
        params.append(bus)
    if can_id is not None:
        sql += " AND can_id = ?"
        params.append(can_id)
    sql += " ORDER BY ts LIMIT ?"
    params.append(limit)

    with closing(connect(db_path)) as conn:
        rows = conn.execute(sql, params).fetchall()
    return [
        {
            "ts": ts,
            "device_id": device_id,
            "boot_id": boot_id,
            "t_us": t_us,
            "bus": BUS_NAMES.get(bus_id, str(bus_id)),
            "can_id": f"0x{cid:08X}" if ext else f"0x{cid:03X}",
            "dir": "tx" if direction else "rx",
            "data": data.hex(" ").upper(),
        }
        for ts, device_id, boot_id, t_us, bus_id, cid, ext, direction, data in rows
    ]


def stats(db_path: str) -> list[dict]:
    with closing(connect(db_path)) as conn:
        rows = conn.execute(
            """
            SELECT b.device_id,
                   COUNT(*)            AS batches,
                   SUM(b.frame_count)  AS frames,
                   MAX(b.received_at)  AS last_seen,
                   (SELECT dropped FROM batches b2
                     WHERE b2.device_id = b.device_id
                     ORDER BY b2.id DESC LIMIT 1) AS dropped_since_boot
            FROM batches b
            GROUP BY b.device_id
            """
        ).fetchall()
    return [
        {"device_id": d, "batches": nb, "frames": nf, "last_seen": ls, "dropped_since_boot": dr}
        for d, nb, nf, ls, dr in rows
    ]