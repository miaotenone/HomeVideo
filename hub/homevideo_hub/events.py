from __future__ import annotations

import json
import os
import sqlite3
from pathlib import Path

from . import storage
from .models import EventCreate

# Snapshot file extensions derived from the uploaded Content-Type.
_SNAPSHOT_EXTENSIONS = {"image/png": ".png", "image/jpeg": ".jpg"}

_UPSERT_EVENT = """
    INSERT INTO events (
        event_id, device_id, event_type, started_at, received_at,
        time_trusted, device_boot_id, device_started_ms, model_version,
        config_version, arm_mode, detections_json, status,
        created_at, updated_at
    ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 'active', ?, ?)
    ON CONFLICT(event_id) DO UPDATE SET
        device_id = excluded.device_id,
        event_type = excluded.event_type,
        started_at = excluded.started_at,
        received_at = excluded.received_at,
        time_trusted = excluded.time_trusted,
        device_boot_id = excluded.device_boot_id,
        device_started_ms = excluded.device_started_ms,
        model_version = excluded.model_version,
        config_version = excluded.config_version,
        arm_mode = excluded.arm_mode,
        detections_json = excluded.detections_json,
        updated_at = excluded.updated_at
"""


def upsert_event(
    conn: sqlite3.Connection, payload: EventCreate, now: float
) -> tuple[str, bool]:
    """Insert an event or refresh its metadata; return ``(event_id, created)``.

    ``created`` is true only for the first insert of a given ``event_id``;
    repeats refresh the stored metadata without adding a second row. An
    untrusted device clock never contributes a wall-clock ``started_at``,
    while the hub's own ``now`` is always recorded as ``received_at``.
    """

    started_at = payload.started_at if payload.time_trusted else None
    detections_json = json.dumps(
        [detection.model_dump(mode="json") for detection in payload.detections]
    )

    with conn:
        created = (
            conn.execute(
                "SELECT 1 FROM events WHERE event_id = ?", (payload.event_id,)
            ).fetchone()
            is None
        )
        conn.execute(
            _UPSERT_EVENT,
            (
                payload.event_id,
                payload.device_id,
                payload.event_type,
                started_at,
                now,
                int(payload.time_trusted),
                payload.device_boot_id,
                payload.device_started_ms,
                payload.model_version,
                payload.config_version,
                payload.arm_mode,
                detections_json,
                now,
                now,
            ),
        )

    return payload.event_id, created


def _snapshot_extension(content_type: str | None) -> str:
    """Map a Content-Type to the snapshot file extension (``.bin`` fallback)."""

    media_type = (content_type or "").split(";", 1)[0].strip().lower()
    return _SNAPSHOT_EXTENSIONS.get(media_type, ".bin")


def store_snapshot(
    conn: sqlite3.Connection,
    event_id: str,
    dest: Path,
    sha256: str,
    size_bytes: int,
    content_type: str,
    now: float,
) -> str:
    """Publish the staged snapshot at ``dest`` and record it.

    ``dest`` is the temporary file the caller streamed the upload into; on a
    first arrival it is renamed to its canonical name ``f"{event_id}{ext}"``
    and a ``snapshots`` row is inserted. If a row already exists for the
    event — whatever its checksum — the upload is a duplicate: the staged
    file is deleted and ``"duplicate"`` is returned without writing a second
    file or row. Returns ``"stored"`` when the snapshot is published.
    """

    with conn:
        existing = conn.execute(
            "SELECT 1 FROM snapshots WHERE event_id = ?", (event_id,)
        ).fetchone()
        if existing is not None:
            storage.safe_delete(dest)
            return "duplicate"
        final = dest.with_name(f"{event_id}{_snapshot_extension(content_type)}")
        os.replace(dest, final)
        conn.execute(
            "INSERT INTO snapshots "
            "(event_id, path, content_type, sha256, size_bytes, created_at) "
            "VALUES (?, ?, ?, ?, ?, ?)",
            (event_id, str(final), content_type, sha256, size_bytes, now),
        )

    return "stored"
