from __future__ import annotations

import sqlite3
from typing import Literal

from .models import DeviceReport

# The four design categories (设备失联 / 视频故障 / 检测失效 / 录像失败) are
# carried by four independent fields, each with its own vocabulary — they are
# not values of one shared enum:
#   * ``link``       device reachability          ``online``/``offline``/``unknown``
#   * ``video``      camera pipeline flag         ``ok``/``failed``/``unknown``
#   * ``recording``  recorder flag                ``ok``/``failed``/``unknown``
#   * ``detection``  per-detector liveness        ``ok``/``stale``/``unknown``
# Only ``video`` and ``recording`` can be ``failed``, because only they have an
# explicit hardware flag (``video_ok`` / ``recording_ok``) to report it. A
# detector has no separate failure signal, so a detector whose completions have
# aged past the staleness threshold *is* the representation of detection
# failure (检测失效): it surfaces as ``stale``, never ``failed``.
LinkState = Literal["online", "offline", "unknown"]
FlagState = Literal["ok", "failed", "unknown"]
DetectionState = Literal["ok", "stale", "unknown"]

# The three detectors a device reports on, mapped to their devices-table
# columns. Staleness is derived from the device's own millisecond uptime
# clock, never the hub wall clock, so hub/device skew cannot misjudge a
# detector.
_DETECTOR_COLUMNS: dict[str, str] = {
    "person": "person_last_detection_ms",
    "fire": "fire_last_detection_ms",
    "smoke": "smoke_last_detection_ms",
}

_UPSERT_DEVICE = """
    INSERT INTO devices (
        device_id, boot_id, uptime_ms, last_frame_ms, last_encode_ms,
        person_last_detection_ms, fire_last_detection_ms,
        smoke_last_detection_ms, video_ok, recording_ok, storage_free_bytes,
        arm_applied, arm_applied_version, time_trusted, last_seen_at
    ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    ON CONFLICT(device_id) DO UPDATE SET
        boot_id = excluded.boot_id,
        uptime_ms = excluded.uptime_ms,
        last_frame_ms = excluded.last_frame_ms,
        last_encode_ms = excluded.last_encode_ms,
        person_last_detection_ms = excluded.person_last_detection_ms,
        fire_last_detection_ms = excluded.fire_last_detection_ms,
        smoke_last_detection_ms = excluded.smoke_last_detection_ms,
        video_ok = excluded.video_ok,
        recording_ok = excluded.recording_ok,
        storage_free_bytes = excluded.storage_free_bytes,
        arm_applied = excluded.arm_applied,
        arm_applied_version = excluded.arm_applied_version,
        time_trusted = excluded.time_trusted,
        last_seen_at = excluded.last_seen_at
"""


def record_report(
    conn: sqlite3.Connection, report: DeviceReport, now: float
) -> None:
    """Persist a heartbeat, stamping its arrival with the hub clock.

    A device's latest report is the single source of truth for its health, so
    a repeat heartbeat replaces the previous one for the same ``device_id``.
    ``last_seen_at`` is always the hub's ``now`` — the value link liveness is
    measured against.
    """

    with conn:
        conn.execute(
            _UPSERT_DEVICE,
            (
                report.device_id,
                report.boot_id,
                report.uptime_ms,
                report.last_frame_ms,
                report.last_encode_ms,
                report.person_last_detection_ms,
                report.fire_last_detection_ms,
                report.smoke_last_detection_ms,
                None if report.video_ok is None else int(report.video_ok),
                None if report.recording_ok is None else int(report.recording_ok),
                report.storage_free_bytes,
                report.arm_applied,
                report.arm_applied_version,
                int(report.time_trusted),
                now,
            ),
        )


def _link_state(row: sqlite3.Row | None, now: float, timeout_s: int) -> LinkState:
    """Liveness from the hub clock: never seen is ``unknown``, not offline."""

    if row is None or row["last_seen_at"] is None:
        return "unknown"
    return "online" if now - row["last_seen_at"] <= timeout_s else "offline"


def _flag_state(row: sqlite3.Row | None, column: str) -> FlagState:
    """Map a boolean report flag to ``ok``/``failed``/``unknown``.

    A missing row or an unset flag is ``unknown`` — an absent report is never
    treated as healthy.
    """

    if row is None or row[column] is None:
        return "unknown"
    return "ok" if row[column] else "failed"


def _detector_state(
    row: sqlite3.Row | None, column: str, stale_s: int
) -> DetectionState:
    """Judge one detector from its own uptime delta.

    ``uptime_ms - last_detection_ms`` measures how long ago the detector last
    completed, independent of any wall clock. A missing row or value is
    ``unknown``. A gap strictly greater than ``stale_s`` marks the detector
    ``stale`` — this is how 检测失效 (detection failure) is represented; there
    is no separate ``failed`` state for a detector. Any younger gap is ``ok``,
    including a gap exactly equal to ``stale_s``.
    """

    if row is None or row[column] is None or row["uptime_ms"] is None:
        return "unknown"
    age_ms = row["uptime_ms"] - row[column]
    return "stale" if age_ms > stale_s * 1000 else "ok"


def device_health(
    conn: sqlite3.Connection,
    device_id: str,
    now: float,
    *,
    timeout_s: int,
    stale_s: int,
) -> dict:
    """Return the derived health of one device.

    The result is JSON-serializable and always has the same shape, whether or
    not the device has ever checked in: ``device_id``, ``link``
    (``online``/``offline``/``unknown``), ``video`` (``ok``/``failed``/
    ``unknown``), ``detection`` (one ``ok``/``stale``/``unknown`` state per
    detector — a stalled detector is ``stale``, never ``failed``),
    ``recording`` (``ok``/``failed``/``unknown``) and the recorded
    ``last_seen_at``.
    """

    row = conn.execute(
        "SELECT * FROM devices WHERE device_id = ?", (device_id,)
    ).fetchone()

    return {
        "device_id": device_id,
        "link": _link_state(row, now, timeout_s),
        "video": _flag_state(row, "video_ok"),
        "detection": {
            name: _detector_state(row, column, stale_s)
            for name, column in _DETECTOR_COLUMNS.items()
        },
        "recording": _flag_state(row, "recording_ok"),
        "last_seen_at": row["last_seen_at"] if row is not None else None,
    }


def overview(
    conn: sqlite3.Connection,
    now: float,
    *,
    timeout_s: int,
    stale_s: int,
) -> list[dict]:
    """Return the derived health of every known device, ordered by id."""

    rows = conn.execute(
        "SELECT device_id FROM devices ORDER BY device_id ASC"
    ).fetchall()
    return [
        device_health(
            conn, row["device_id"], now, timeout_s=timeout_s, stale_s=stale_s
        )
        for row in rows
    ]
