from __future__ import annotations

import json
import os
import sqlite3
from pathlib import Path

from . import storage
from .models import EventClose, EventCreate

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


class SegmentConflict(Exception):
    """Raised when a ``(event_id, seq)`` row exists with another checksum."""


def get_segment(
    conn: sqlite3.Connection, event_id: str, seq: int
) -> sqlite3.Row | None:
    """Return the stored segment row for ``(event_id, seq)``, if any."""

    return conn.execute(
        "SELECT * FROM segments WHERE event_id = ? AND seq = ?",
        (event_id, seq),
    ).fetchone()


def _event_stored_bytes(conn: sqlite3.Connection, event_id: str) -> int:
    """Total bytes this event already occupies across segments and snapshot."""

    segments_total = conn.execute(
        "SELECT COALESCE(SUM(size_bytes), 0) FROM segments WHERE event_id = ?",
        (event_id,),
    ).fetchone()[0]
    snapshot = conn.execute(
        "SELECT size_bytes FROM snapshots WHERE event_id = ?", (event_id,)
    ).fetchone()
    return int(segments_total) + (snapshot["size_bytes"] if snapshot else 0)


def store_segment(
    conn: sqlite3.Connection,
    event_id: str,
    seq: int,
    dest: Path,
    sha256: str,
    size_bytes: int,
    now: float,
    max_event_bytes: int,
) -> str:
    """Publish the staged segment at ``dest`` and record it.

    ``dest`` is the temporary file the caller streamed the upload into; on a
    first arrival it is renamed atomically to ``f"{event_id}_{seq}.bin"`` and
    a ``segments`` row is inserted. Re-uploading the same ``(event_id, seq)``
    with the same checksum is an idempotent duplicate: the staged file is
    deleted and ``"duplicate"`` is returned. A different checksum for the
    same sequence raises :class:`SegmentConflict` and is never overwritten.
    The event's cumulative stored bytes (segments plus snapshot) must stay
    within ``max_event_bytes``; otherwise :class:`storage.InsufficientStorage`
    is raised. Every failure path deletes the staged file.
    """

    existing = get_segment(conn, event_id, seq)
    if existing is not None:
        storage.safe_delete(dest)
        if existing["sha256"] == sha256:
            return "duplicate"
        raise SegmentConflict(
            f"segment {event_id}/{seq} already stored with a different checksum"
        )

    if _event_stored_bytes(conn, event_id) + size_bytes > max_event_bytes:
        storage.safe_delete(dest)
        raise storage.InsufficientStorage(
            f"event {event_id} would exceed max_event_bytes={max_event_bytes}"
        )

    final = dest.with_name(f"{event_id}_{seq}.bin")
    os.replace(dest, final)
    with conn:
        conn.execute(
            "INSERT INTO segments "
            "(event_id, seq, path, sha256, size_bytes, created_at, updated_at) "
            "VALUES (?, ?, ?, ?, ?, ?, ?)",
            (event_id, seq, str(final), sha256, size_bytes, now, now),
        )

    return "stored"


def _row_to_dict(row: sqlite3.Row) -> dict:
    """Copy a ``sqlite3.Row`` into a plain, JSON-serializable dict."""

    return {key: row[key] for key in row.keys()}


def _serialize_event(row: sqlite3.Row) -> dict:
    """Convert an ``events`` row into a JSON-serializable dict.

    ``time_trusted`` is normalized to a real ``bool`` and the stored
    ``detections_json`` text is decoded into a ``detections`` list. No
    ``sqlite3.Row`` ever leaks past this point.
    """

    event = _row_to_dict(row)
    event["time_trusted"] = bool(event["time_trusted"])
    raw_detections = event.pop("detections_json", None)
    event["detections"] = json.loads(raw_detections) if raw_detections else []
    return event


def close_event(
    conn: sqlite3.Connection,
    event_id: str,
    payload: EventClose,
    now: float,
) -> None:
    """Mark an event ``closed`` and stamp ``closed_at`` from the hub clock.

    End information is persisted alongside the closure: ``ended_at`` follows
    the same trust rule as ``started_at`` — an untrusted device clock never
    contributes a wall-clock time, so it is stored as ``NULL`` — while
    ``duration_ms`` is always recorded because it is the only surviving length
    measure for a time-untrusted event. Closing an event with no segments (or
    one already closed) is allowed and idempotent.
    """

    ended_at = payload.ended_at if payload.time_trusted else None
    with conn:
        conn.execute(
            "UPDATE events SET status = 'closed', closed_at = ?, updated_at = ?, "
            "ended_at = ?, duration_ms = ? WHERE event_id = ?",
            (now, now, ended_at, payload.duration_ms, event_id),
        )


def get_event(conn: sqlite3.Connection, event_id: str) -> dict | None:
    """Return the full detail for ``event_id``, or ``None`` when unknown.

    The result holds only JSON-serializable native types: ``snapshot`` is a
    ``dict`` or ``None`` and ``segments`` is a ``list[dict]`` ordered by
    ``seq`` ascending.
    """

    row = conn.execute(
        "SELECT * FROM events WHERE event_id = ?", (event_id,)
    ).fetchone()
    if row is None:
        return None

    event = _serialize_event(row)
    snapshot = conn.execute(
        "SELECT * FROM snapshots WHERE event_id = ?", (event_id,)
    ).fetchone()
    event["snapshot"] = _row_to_dict(snapshot) if snapshot is not None else None
    event["segments"] = [
        _row_to_dict(segment)
        for segment in conn.execute(
            "SELECT * FROM segments WHERE event_id = ? ORDER BY seq ASC",
            (event_id,),
        ).fetchall()
    ]
    return event


def list_events(
    conn: sqlite3.Connection,
    *,
    event_type: str | None = None,
    status: str | None = None,
    limit: int = 50,
    offset: int = 0,
) -> list[dict]:
    """Return event summaries, most recently received first.

    ``event_type`` and ``status`` narrow the result; ``limit``/``offset`` page
    through it. Untrusted-time events still sort by ``received_at`` and carry
    ``started_at: None``. Each item is a JSON-serializable dict of the event's
    own columns (no snapshot or segments).
    """

    clauses: list[str] = []
    params: list[object] = []
    if event_type is not None:
        clauses.append("event_type = ?")
        params.append(event_type)
    if status is not None:
        clauses.append("status = ?")
        params.append(status)
    where = f" WHERE {' AND '.join(clauses)}" if clauses else ""

    rows = conn.execute(
        f"SELECT * FROM events{where} "
        "ORDER BY received_at DESC, event_id ASC LIMIT ? OFFSET ?",
        (*params, limit, offset),
    ).fetchall()
    return [_serialize_event(row) for row in rows]
