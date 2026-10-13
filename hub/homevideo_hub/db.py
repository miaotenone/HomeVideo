from __future__ import annotations

import sqlite3
from pathlib import Path

SCHEMA_VERSION = "1"

_SCHEMA_STATEMENTS: tuple[str, ...] = (
    """
    CREATE TABLE IF NOT EXISTS meta (
        key TEXT PRIMARY KEY,
        value TEXT
    )
    """,
    """
    CREATE TABLE IF NOT EXISTS devices (
        device_id TEXT PRIMARY KEY,
        boot_id TEXT,
        uptime_ms INTEGER,
        last_frame_ms INTEGER,
        last_encode_ms INTEGER,
        person_last_detection_ms INTEGER,
        fire_last_detection_ms INTEGER,
        smoke_last_detection_ms INTEGER,
        video_ok INTEGER,
        recording_ok INTEGER,
        storage_free_bytes INTEGER,
        arm_applied TEXT,
        arm_applied_version INTEGER,
        time_trusted INTEGER,
        last_seen_at REAL
    )
    """,
    """
    CREATE TABLE IF NOT EXISTS events (
        event_id TEXT PRIMARY KEY,
        device_id TEXT NOT NULL,
        event_type TEXT NOT NULL,
        started_at REAL,
        received_at REAL NOT NULL,
        time_trusted INTEGER NOT NULL DEFAULT 0,
        device_boot_id TEXT,
        device_started_ms INTEGER,
        model_version TEXT,
        config_version INTEGER,
        arm_mode TEXT,
        detections_json TEXT,
        status TEXT NOT NULL DEFAULT 'active',
        created_at REAL NOT NULL,
        updated_at REAL NOT NULL,
        closed_at REAL
    )
    """,
    """
    CREATE TABLE IF NOT EXISTS snapshots (
        event_id TEXT PRIMARY KEY
            REFERENCES events(event_id) ON DELETE CASCADE,
        path TEXT NOT NULL,
        content_type TEXT,
        sha256 TEXT NOT NULL,
        size_bytes INTEGER NOT NULL,
        created_at REAL NOT NULL
    )
    """,
    """
    CREATE TABLE IF NOT EXISTS segments (
        event_id TEXT NOT NULL
            REFERENCES events(event_id) ON DELETE CASCADE,
        seq INTEGER NOT NULL,
        path TEXT NOT NULL,
        sha256 TEXT NOT NULL,
        size_bytes INTEGER NOT NULL,
        created_at REAL NOT NULL,
        updated_at REAL NOT NULL,
        UNIQUE (event_id, seq)
    )
    """,
    """
    CREATE TABLE IF NOT EXISTS arm_config (
        id INTEGER PRIMARY KEY CHECK (id = 1),
        mode TEXT NOT NULL DEFAULT 'unknown',
        config_version INTEGER NOT NULL DEFAULT 0,
        updated_at REAL
    )
    """,
    """
    CREATE TABLE IF NOT EXISTS arm_ack (
        device_id TEXT PRIMARY KEY,
        applied_mode TEXT NOT NULL,
        applied_version INTEGER NOT NULL,
        updated_at REAL
    )
    """,
    """
    CREATE TABLE IF NOT EXISTS deletions (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        event_id TEXT NOT NULL,
        reason TEXT NOT NULL,
        bytes_freed INTEGER NOT NULL,
        created_at REAL NOT NULL
    )
    """,
)


def connect(db_path: Path) -> sqlite3.Connection:
    """Open a SQLite connection with the hub's required pragmas."""

    conn = sqlite3.connect(db_path)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA foreign_keys=ON")
    return conn


def init_db(conn: sqlite3.Connection) -> None:
    """Create every hub table and stamp the schema version (idempotent)."""

    with conn:
        for statement in _SCHEMA_STATEMENTS:
            conn.execute(statement)
        conn.execute(
            "INSERT INTO meta(key, value) VALUES ('schema_version', ?) "
            "ON CONFLICT(key) DO UPDATE SET value = excluded.value",
            (SCHEMA_VERSION,),
        )
