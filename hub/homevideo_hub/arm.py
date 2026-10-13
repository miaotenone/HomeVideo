from __future__ import annotations

import sqlite3
from typing import Literal

from .models import ArmAck, ArmMode

# The hub's "not initialized" arm state. It is never requested by a client and
# never reported as an armed mode; until the first ``PUT /api/v1/arm`` the
# desired mode is ``unknown`` at version ``0``.
UNKNOWN: ArmMode = "unknown"

# ``arm_config`` holds exactly one row (``id = 1``) — the desired mode and the
# version the hub has published. ``arm_ack`` holds one row per device with the
# last configuration that device confirmed applying.
_UPSERT_CONFIG = """
    INSERT INTO arm_config (id, mode, config_version, updated_at)
    VALUES (1, ?, ?, ?)
    ON CONFLICT(id) DO UPDATE SET
        mode = excluded.mode,
        config_version = excluded.config_version,
        updated_at = excluded.updated_at
"""
_UPSERT_ACK = """
    INSERT INTO arm_ack (device_id, applied_mode, applied_version, updated_at)
    VALUES (?, ?, ?, ?)
    ON CONFLICT(device_id) DO UPDATE SET
        applied_mode = excluded.applied_mode,
        applied_version = excluded.applied_version,
        updated_at = excluded.updated_at
"""


def get_desired(conn: sqlite3.Connection) -> tuple[ArmMode, int]:
    """Return the desired arm mode and its config version.

    A missing ``arm_config`` row means the hub has never been armed, so the
    initial state is ``("unknown", 0)`` — "not initialized", never an armed
    mode.
    """

    row = conn.execute(
        "SELECT mode, config_version FROM arm_config WHERE id = 1"
    ).fetchone()
    if row is None:
        return (UNKNOWN, 0)
    return (row["mode"], row["config_version"])


def set_desired(
    conn: sqlite3.Connection, mode: Literal["home", "away"], now: float
) -> tuple[ArmMode, int]:
    """Set the desired arm mode and return it with a bumped config version.

    The version increases monotonically for the hub's lifetime so a device can
    tell which configuration it is acknowledging; every call bumps it. The
    single persisted row means the last published configuration survives a hub
    restart.
    """

    _, version = get_desired(conn)
    version += 1
    with conn:
        conn.execute(_UPSERT_CONFIG, (mode, version, now))
    return (mode, version)


def _confirms(
    desired_mode: ArmMode,
    desired_version: int,
    applied_mode: ArmMode,
    applied_version: int,
) -> bool:
    """Whether an acknowledgement covers the currently desired configuration.

    An acknowledgement is in force only when the hub has actually been armed
    (desired mode is not ``unknown``), the device applied the same mode the hub
    wants, and its applied version is not behind the desired version. A stale
    acknowledgement — from before the latest change — never becomes effective,
    which is what makes "已生效" mean the device really is running the current
    configuration.
    """

    if desired_mode == UNKNOWN:
        return False
    return applied_mode == desired_mode and applied_version >= desired_version


def record_ack(conn: sqlite3.Connection, ack: ArmAck, now: float) -> bool:
    """Record a device's latest acknowledgement and report if it took effect.

    The device's previous acknowledgement is replaced, so ``arm_ack`` always
    holds what each device last applied. The return value is ``True`` when that
    acknowledgement covers the current desired version (see ``_confirms``).
    """

    with conn:
        conn.execute(
            _UPSERT_ACK,
            (ack.device_id, ack.applied_mode, ack.applied_version, now),
        )
    desired_mode, desired_version = get_desired(conn)
    return _confirms(
        desired_mode, desired_version, ack.applied_mode, ack.applied_version
    )


def _latest_ack(conn: sqlite3.Connection) -> sqlite3.Row | None:
    """The device acknowledgement describing the most advanced applied state."""

    return conn.execute(
        "SELECT device_id, applied_mode, applied_version, updated_at "
        "FROM arm_ack "
        "ORDER BY applied_version DESC, updated_at DESC, device_id ASC "
        "LIMIT 1"
    ).fetchone()


def arm_status(conn: sqlite3.Connection) -> dict:
    """Summarize the desired versus applied arm configuration.

    ``desired`` is what the hub has published; ``applied`` is the most advanced
    device acknowledgement, or ``None`` before any device has confirmed.
    ``effective`` is ``True`` only when at least one device has
    confirmed the current desired version — the hub never reports ``unknown``
    as an armed mode and never reports a pending or stale acknowledgement as
    effective. ``acked_devices`` lists the devices confirming the current
    version, so devices that have not acknowledged are those absent from it.
    """

    desired_mode, desired_version = get_desired(conn)
    rows = conn.execute(
        "SELECT device_id, applied_mode, applied_version "
        "FROM arm_ack ORDER BY device_id ASC"
    ).fetchall()
    acked_devices = [
        row["device_id"]
        for row in rows
        if _confirms(
            desired_mode,
            desired_version,
            row["applied_mode"],
            row["applied_version"],
        )
    ]

    latest = _latest_ack(conn)
    applied = (
        None
        if latest is None
        else {
            "mode": latest["applied_mode"],
            "config_version": latest["applied_version"],
            "device_id": latest["device_id"],
            "updated_at": latest["updated_at"],
        }
    )

    return {
        "desired": {"mode": desired_mode, "config_version": desired_version},
        "applied": applied,
        "effective": bool(acked_devices),
        "acked_devices": acked_devices,
    }
