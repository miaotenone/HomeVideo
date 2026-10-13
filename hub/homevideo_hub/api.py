from __future__ import annotations

import time

from fastapi import FastAPI, Request
from fastapi.responses import JSONResponse

from .config import Settings, load_settings
from .db import connect, init_db
from .events import upsert_event
from .models import EventCreate


def create_app(settings: Settings | None = None) -> FastAPI:
    """Build the hub application: create directories, schema and state.

    The SQLite connection and resolved settings are attached to
    ``app.state`` inside the factory so tests and later tasks can reach them
    without relying on a startup event.
    """

    if settings is None:
        settings = load_settings()

    for directory in (
        settings.data_dir,
        settings.media_dir,
        settings.snapshot_dir,
        settings.segment_dir,
        settings.notification_dir,
    ):
        directory.mkdir(parents=True, exist_ok=True)

    conn = connect(settings.db_path)
    init_db(conn)

    app = FastAPI(title="HomeVideo Hub")
    app.state.settings = settings
    app.state.conn = conn

    @app.post("/api/v1/events")
    def create_event(payload: EventCreate, request: Request) -> JSONResponse:
        event_id, created = upsert_event(
            request.app.state.conn, payload, time.time()
        )
        return JSONResponse(
            status_code=201 if created else 200,
            content={"event_id": event_id, "created": created},
        )

    return app
