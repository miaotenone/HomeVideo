from __future__ import annotations

import json
import sqlite3

from .models import EventCreate

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
