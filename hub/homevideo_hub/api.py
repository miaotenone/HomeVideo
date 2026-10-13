from __future__ import annotations

import time
import uuid

from fastapi import FastAPI, Request
from fastapi.responses import JSONResponse

from . import storage
from .config import Settings, load_settings
from .db import connect, init_db
from .events import (
    SegmentConflict,
    store_segment,
    store_snapshot,
    upsert_event,
)
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

    @app.post("/api/v1/events/{event_id}/snapshot")
    async def upload_snapshot(event_id: str, request: Request) -> JSONResponse:
        conn = request.app.state.conn
        snapshot_dir = request.app.state.settings.snapshot_dir

        if (
            conn.execute(
                "SELECT 1 FROM events WHERE event_id = ?", (event_id,)
            ).fetchone()
            is None
        ):
            return JSONResponse(
                status_code=404, content={"detail": "event not found"}
            )

        content_type = request.headers.get("content-type", "")
        staged = snapshot_dir / f".{event_id}.{uuid.uuid4().hex}.upload"
        try:
            size_bytes, sha256 = await storage.write_stream(
                request.stream(), staged
            )
            status = store_snapshot(
                conn,
                event_id,
                staged,
                sha256,
                size_bytes,
                content_type,
                time.time(),
            )
        except BaseException:
            storage.safe_delete(staged)
            raise

        return JSONResponse(
            status_code=201 if status == "stored" else 200,
            content={"event_id": event_id, "status": status},
        )

    @app.put("/api/v1/events/{event_id}/segments/{seq}")
    async def upload_segment(
        event_id: str, seq: int, request: Request
    ) -> JSONResponse:
        conn = request.app.state.conn
        settings = request.app.state.settings
        segment_dir = settings.segment_dir

        if (
            conn.execute(
                "SELECT 1 FROM events WHERE event_id = ?", (event_id,)
            ).fetchone()
            is None
        ):
            return JSONResponse(
                status_code=404, content={"detail": "event not found"}
            )

        declared = request.headers.get("x-segment-sha256", "")
        staged = segment_dir / f".{event_id}.{seq}.{uuid.uuid4().hex}.upload"
        try:
            size_bytes, sha256 = await storage.write_stream(
                request.stream(), staged
            )
            if declared.strip().lower() != sha256:
                storage.safe_delete(staged)
                return JSONResponse(
                    status_code=400, content={"detail": "checksum mismatch"}
                )
            storage.ensure_capacity(
                segment_dir, size_bytes, settings.metadata_reserve_bytes
            )
            try:
                status = store_segment(
                    conn,
                    event_id,
                    seq,
                    staged,
                    sha256,
                    size_bytes,
                    time.time(),
                    settings.max_event_bytes,
                )
            except SegmentConflict:
                return JSONResponse(
                    status_code=409, content={"detail": "segment conflict"}
                )
        except storage.InsufficientStorage:
            storage.safe_delete(staged)
            return JSONResponse(
                status_code=507, content={"detail": "insufficient storage"}
            )
        except BaseException:
            storage.safe_delete(staged)
            raise

        return JSONResponse(
            status_code=201 if status == "stored" else 200,
            content={
                "event_id": event_id,
                "seq": seq,
                "status": status,
                "sha256": sha256,
            },
        )

    return app
