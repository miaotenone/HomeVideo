from __future__ import annotations

import sqlite3
from pathlib import Path

from . import storage
from .config import Settings

# Every retention deletion is recorded with this reason.
_REASON_EXPIRED = "retention_expired"

_SECONDS_PER_DAY = 86_400.0


def _delete_event(
    conn: sqlite3.Connection, event_id: str, now: float
) -> int:
    """Delete one event's media files, rows and record the deletion.

    Media files (the snapshot and every segment) are removed from disk
    first, then the event row — whose ``snapshots`` and ``segments`` rows
    cascade away — and finally a ``deletions`` record is written. Returns
    the total recorded bytes freed.
    """

    snapshot = conn.execute(
        "SELECT path, size_bytes FROM snapshots WHERE event_id = ?",
        (event_id,),
    ).fetchone()
    segments = conn.execute(
        "SELECT path, size_bytes FROM segments WHERE event_id = ?",
        (event_id,),
    ).fetchall()

    bytes_freed = 0
    files: list[sqlite3.Row] = []
    if snapshot is not None:
        files.append(snapshot)
    files.extend(segments)
    for row in files:
        storage.safe_delete(Path(row["path"]))
        bytes_freed += int(row["size_bytes"])

    with conn:
        conn.execute("DELETE FROM events WHERE event_id = ?", (event_id,))
        conn.execute(
            "INSERT INTO deletions (event_id, reason, bytes_freed, created_at) "
            "VALUES (?, ?, ?, ?)",
            (event_id, _REASON_EXPIRED, bytes_freed, now),
        )

    return bytes_freed


def run_cleanup(
    conn: sqlite3.Connection, settings: Settings, now: float
) -> dict:
    """Expire closed events older than the retention window.

    An event is deleted only when it is ``closed`` and its ``received_at``
    precedes ``now - retention_days * 86400``; active events are never
    touched. Each expired event's media files and rows are removed and the
    deletion recorded. Returns ``{"deleted_events", "bytes_freed"}``.
    """

    cutoff = now - settings.retention_days * _SECONDS_PER_DAY
    expired = conn.execute(
        "SELECT event_id FROM events "
        "WHERE status = 'closed' AND received_at < ?",
        (cutoff,),
    ).fetchall()

    deleted_events = 0
    bytes_freed = 0
    for row in expired:
        bytes_freed += _delete_event(conn, row["event_id"], now)
        deleted_events += 1

    return {"deleted_events": deleted_events, "bytes_freed": bytes_freed}
