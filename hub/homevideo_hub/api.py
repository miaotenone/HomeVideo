from __future__ import annotations

import logging
import sqlite3
import time
import uuid
from collections.abc import Iterator
from typing import Annotated

from fastapi import Depends, FastAPI, Query, Request
from fastapi.responses import JSONResponse

from . import arm, health, notify, retention, storage
from .config import Settings, load_settings
from .db import connect, init_db
from .events import (
    SegmentConflict,
    close_event,
    get_event,
    list_events,
    store_segment,
    store_snapshot,
    upsert_event,
)
from .models import ArmAck, ArmRequest, DeviceReport, EventClose, EventCreate

logger = logging.getLogger(__name__)


def _request_conn(request: Request) -> Iterator[sqlite3.Connection]:
    """Yield a per-request SQLite connection, closed after the response.

    FastAPI runs the sync routes (and this generator dependency) in its
    threadpool, so a single shared connection would let concurrent requests
    interleave one implicit transaction with another's ``with conn:`` commit
    or rollback. Each request gets its own connection instead; it is opened
    through :func:`homevideo_hub.db.connect` so it still carries the
    ``row_factory``, WAL journal and ``PRAGMA foreign_keys=ON``. The
    long-lived ``app.state.conn`` from startup remains for tests and tooling.
    """

    conn = connect(request.app.state.settings.db_path)
    try:
        yield conn
    finally:
        conn.close()


# A request-scoped connection, injected by FastAPI into every sync route.
Conn = Annotated[sqlite3.Connection, Depends(_request_conn)]


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
    app.state.notifier = notify.get_notifier(settings)

    @app.post("/api/v1/device/heartbeat")
    def device_heartbeat(
        payload: DeviceReport, request: Request, conn: Conn
    ) -> JSONResponse:
        health.record_report(conn, payload, time.time())
        return JSONResponse(
            status_code=200,
            content={"device_id": payload.device_id, "status": "ok"},
        )

    @app.get("/api/v1/devices/{device_id}/health")
    def device_health_route(
        device_id: str, request: Request, conn: Conn
    ) -> JSONResponse:
        settings = request.app.state.settings
        state = health.device_health(
            conn,
            device_id,
            time.time(),
            timeout_s=settings.heartbeat_timeout_seconds,
            stale_s=settings.detection_stale_seconds,
        )
        return JSONResponse(content=state)

    @app.get("/api/v1/health")
    def health_overview(request: Request, conn: Conn) -> JSONResponse:
        settings = request.app.state.settings
        devices = health.overview(
            conn,
            time.time(),
            timeout_s=settings.heartbeat_timeout_seconds,
            stale_s=settings.detection_stale_seconds,
        )
        return JSONResponse(content={"devices": devices})

    @app.get("/api/v1/arm")
    def get_arm(conn: Conn) -> JSONResponse:
        mode, config_version = arm.get_desired(conn)
        return JSONResponse(
            content={"mode": mode, "config_version": config_version}
        )

    @app.put("/api/v1/arm")
    def set_arm(payload: ArmRequest, conn: Conn) -> JSONResponse:
        mode, config_version = arm.set_desired(conn, payload.mode, time.time())
        return JSONResponse(
            content={"mode": mode, "config_version": config_version}
        )

    @app.post("/api/v1/arm/ack")
    def arm_ack(payload: ArmAck, conn: Conn) -> JSONResponse:
        effective = arm.record_ack(conn, payload, time.time())
        return JSONResponse(
            status_code=200, content={"status": "ok", "effective": effective}
        )

    @app.get("/api/v1/arm/status")
    def arm_status(conn: Conn) -> JSONResponse:
        return JSONResponse(content=arm.arm_status(conn))

    @app.post("/api/v1/events")
    def create_event(
        payload: EventCreate, request: Request, conn: Conn
    ) -> JSONResponse:
        event_id, created = upsert_event(conn, payload, time.time())
        if created:
            try:
                request.app.state.notifier.notify(
                    notify.build_notification(payload)
                )
            except Exception:
                logger.exception("notification failed for event %s", event_id)
        return JSONResponse(
            status_code=201 if created else 200,
            content={"event_id": event_id, "created": created},
        )

    @app.post("/api/v1/events/{event_id}/snapshot")
    async def upload_snapshot(
        event_id: str, request: Request, conn: Conn
    ) -> JSONResponse:
        settings = request.app.state.settings
        snapshot_dir = settings.snapshot_dir

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
            storage.ensure_capacity(
                snapshot_dir, size_bytes, settings.metadata_reserve_bytes
            )
            status = store_snapshot(
                conn,
                event_id,
                staged,
                sha256,
                size_bytes,
                content_type,
                time.time(),
                settings.max_event_bytes,
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
            content={"event_id": event_id, "status": status},
        )

    @app.put("/api/v1/events/{event_id}/segments/{seq}")
    async def upload_segment(
        event_id: str, seq: int, request: Request, conn: Conn
    ) -> JSONResponse:
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

    @app.post("/api/v1/events/{event_id}/close")
    def close_event_route(
        event_id: str, payload: EventClose, conn: Conn
    ) -> JSONResponse:
        close_event(conn, event_id, payload, time.time())
        return JSONResponse(
            status_code=200,
            content={"event_id": event_id, "status": "closed"},
        )

    @app.get("/api/v1/events")
    def query_events(
        conn: Conn,
        event_type: str | None = None,
        status: str | None = None,
        limit: Annotated[int, Query(ge=1, le=500)] = 50,
        offset: Annotated[int, Query(ge=0)] = 0,
    ) -> JSONResponse:
        items = list_events(
            conn,
            event_type=event_type,
            status=status,
            limit=limit,
            offset=offset,
        )
        return JSONResponse(
            content={"items": items, "limit": limit, "offset": offset}
        )

    @app.get("/api/v1/events/{event_id}")
    def event_detail(event_id: str, conn: Conn) -> JSONResponse:
        event = get_event(conn, event_id)
        if event is None:
            return JSONResponse(
                status_code=404, content={"detail": "event not found"}
            )
        return JSONResponse(content=event)

    @app.post("/api/v1/maintenance/cleanup")
    def maintenance_cleanup(request: Request, conn: Conn) -> JSONResponse:
        result = retention.run_cleanup(
            conn, request.app.state.settings, time.time()
        )
        return JSONResponse(content=result)

    return app
