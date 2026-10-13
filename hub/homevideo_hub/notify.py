"""Pluggable event notifications.

A notification is a small JSON document describing a newly created event. The
default ``"file"`` channel writes one ``{event_id}.json`` file into the
configured notification directory, which is enough for the edge appliance
without adding a database server, message queue or transcoding component.
``get_notifier`` maps a configured name onto an implementation; an unknown
name yields a notifier that raises on delivery so the failure stays inside the
notification path and never reaches the ingest request.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Protocol

from .config import Settings
from .models import EventCreate


def build_notification(payload: EventCreate) -> dict:
    """Construct the JSON-serializable notification for a new event.

    Carries the event identity plus its start time; ``time_trusted`` marks
    whether that start time can be trusted (an untrusted device clock leaves
    ``started_at`` as ``None``).
    """

    return {
        "event_id": payload.event_id,
        "event_type": payload.event_type,
        "device_id": payload.device_id,
        "started_at": payload.started_at,
        "time_trusted": payload.time_trusted,
    }


class Notifier(Protocol):
    """A sink that delivers an event notification."""

    def notify(self, event: dict) -> None:  # pragma: no cover - protocol
        ...


class UnknownNotifier(Exception):
    """Raised when a notifier name has no registered implementation."""


class FileNotifier:
    """Write each notification as ``{event_id}.json`` in ``directory``."""

    def __init__(self, directory: Path) -> None:
        self._directory = directory

    def notify(self, event: dict) -> None:
        path = self._directory / f"{event['event_id']}.json"
        path.write_text(json.dumps(event), encoding="utf-8")


class _UnsupportedNotifier:
    """Placeholder that fails on delivery for an unknown notifier name."""

    def __init__(self, name: str) -> None:
        self._name = name

    def notify(self, event: dict) -> None:
        raise UnknownNotifier(f"unknown notifier: {self._name!r}")


def get_notifier(settings: Settings) -> Notifier:
    """Return the notifier selected by ``settings.notifier``.

    Only ``"file"`` is implemented; any other name returns a notifier whose
    ``notify`` raises :class:`UnknownNotifier`, so a misconfiguration surfaces
    in the notification path rather than the ingest path.
    """

    if settings.notifier == "file":
        return FileNotifier(settings.notification_dir)
    return _UnsupportedNotifier(settings.notifier)
