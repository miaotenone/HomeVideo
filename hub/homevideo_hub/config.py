from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path
from typing import Mapping

DEFAULT_DATA_DIR = "data"
DEFAULT_RETENTION_DAYS = 7
DEFAULT_MAX_EVENT_BYTES = 2_000_000_000
DEFAULT_METADATA_RESERVE_BYTES = 64 * 1024 * 1024
DEFAULT_HEARTBEAT_TIMEOUT_SECONDS = 30
DEFAULT_DETECTION_STALE_SECONDS = 5
DEFAULT_NOTIFIER = "file"


@dataclass(frozen=True)
class Settings:
    """Resolved hub configuration and derived filesystem paths."""

    data_dir: Path
    db_path: Path
    media_dir: Path
    snapshot_dir: Path
    segment_dir: Path
    notification_dir: Path
    retention_days: int
    max_event_bytes: int
    metadata_reserve_bytes: int
    heartbeat_timeout_seconds: int
    detection_stale_seconds: int
    notifier: str


def load_settings(
    data_dir: Path | None = None,
    env: Mapping[str, str] | None = None,
) -> Settings:
    """Build a :class:`Settings` from an explicit data dir and environment.

    An explicitly supplied ``data_dir`` wins; otherwise ``HOMEVIDEO_DATA_DIR``
    is used, falling back to ``data``. Remaining ``HOMEVIDEO_*`` variables
    override the built-in defaults.
    """

    env = os.environ if env is None else env

    if data_dir is None:
        data_dir = env.get("HOMEVIDEO_DATA_DIR", DEFAULT_DATA_DIR)
    data_dir = Path(data_dir)

    media_dir = data_dir / "media"

    return Settings(
        data_dir=data_dir,
        db_path=data_dir / "hub.db",
        media_dir=media_dir,
        snapshot_dir=media_dir / "snapshots",
        segment_dir=media_dir / "segments",
        notification_dir=data_dir / "notifications",
        retention_days=int(
            env.get("HOMEVIDEO_RETENTION_DAYS", DEFAULT_RETENTION_DAYS)
        ),
        max_event_bytes=int(
            env.get("HOMEVIDEO_MAX_EVENT_BYTES", DEFAULT_MAX_EVENT_BYTES)
        ),
        metadata_reserve_bytes=int(
            env.get(
                "HOMEVIDEO_METADATA_RESERVE_BYTES",
                DEFAULT_METADATA_RESERVE_BYTES,
            )
        ),
        heartbeat_timeout_seconds=int(
            env.get(
                "HOMEVIDEO_HEARTBEAT_TIMEOUT_SECONDS",
                DEFAULT_HEARTBEAT_TIMEOUT_SECONDS,
            )
        ),
        detection_stale_seconds=int(
            env.get(
                "HOMEVIDEO_DETECTION_STALE_SECONDS",
                DEFAULT_DETECTION_STALE_SECONDS,
            )
        ),
        notifier=env.get("HOMEVIDEO_NOTIFIER", DEFAULT_NOTIFIER),
    )
