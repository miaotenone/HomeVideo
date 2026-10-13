from __future__ import annotations

from typing import Annotated, Literal

from pydantic import BaseModel, Field

# The device only ever reports these three detection/event kinds.
EventType = Literal["person_intrusion", "fire_suspected", "smoke_suspected"]

# ``event_id`` is used verbatim as an on-disk filename stem (notifications,
# snapshots and segments all derive their names from it), so it must never
# contain a path separator or drive the path out of its directory. The
# allowed alphabet excludes ``/`` and ``\`` outright, which also rules out
# ``..`` traversal and absolute paths.
EventId = Annotated[str, Field(pattern=r"^[A-Za-z0-9._-]{1,128}$")]

# "unknown" is the hub's default before a device has acknowledged a mode.
ArmMode = Literal["home", "away", "unknown"]


class Detection(BaseModel):
    """A single detector output attached to an event."""

    label: EventType
    confidence: float
    bbox: tuple[int, int, int, int] | None = None
    zone: str | None = None


class EventCreate(BaseModel):
    """Metadata the device uploads when an event starts."""

    event_id: EventId
    device_id: str
    event_type: EventType
    started_at: float | None = None
    time_trusted: bool
    device_boot_id: str | None = None
    device_started_ms: int | None = None
    model_version: str
    config_version: int
    arm_mode: ArmMode
    detections: list[Detection]


class EventClose(BaseModel):
    """Metadata the device uploads when an event ends."""

    ended_at: float | None = None
    duration_ms: int | None = None
    time_trusted: bool = False


class DeviceReport(BaseModel):
    """Periodic heartbeat describing a device's health and progress."""

    device_id: str
    boot_id: str | None = None
    uptime_ms: int | None = None
    last_frame_ms: int | None = None
    last_encode_ms: int | None = None
    person_last_detection_ms: int | None = None
    fire_last_detection_ms: int | None = None
    smoke_last_detection_ms: int | None = None
    video_ok: bool | None = None
    recording_ok: bool | None = None
    storage_free_bytes: int | None = None
    arm_applied: ArmMode | None = None
    arm_applied_version: int | None = None
    time_trusted: bool = False


class ArmRequest(BaseModel):
    """Client request to change the desired arm mode."""

    mode: Literal["home", "away"]


class ArmAck(BaseModel):
    """Device acknowledgement that it applied a given arm configuration."""

    device_id: str
    applied_mode: ArmMode
    applied_version: int


class AckResponse(BaseModel):
    """Generic acknowledgement returned by mutating endpoints."""

    status: str
