from __future__ import annotations

import time
import uuid

from fastapi import FastAPI, Request
from fastapi.responses import JSONResponse

from . import arm, health, storage
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

    @app.post("/api/v1/device/heartbeat")
    def device_heartbeat(payload: DeviceReport, request: Request) -> JSONResponse:
        health.record_report(request.app.state.conn, payload, time.time())
        return JSONResponse(
            status_code=200,
            content={"device_id": payload.device_id, "status": "ok"},
        )

    @app.get("/api/v1/devices/{device_id}/health")
    def device_health_route(device_id: str, request: Request) -> JSONResponse:
        settings = request.app.state.settings
        state = health.device_health(
            request.app.state.conn,
            device_id,
            time.time(),
            timeout_s=settings.heartbeat_timeout_seconds,
            stale_s=settings.detection_stale_seconds,
        )
        return JSONResponse(content=state)

    @app.get("/api/v1/health")
    def health_overview(request: Request) -> JSONResponse:
        settings = request.app.state.settings
        devices = health.overview(
            request.app.state.conn,
            time.time(),
            timeout_s=settings.heartbeat_timeout_seconds,
            stale_s=settings.detection_stale_seconds,
        )
        return JSONResponse(content={"devices": devices})

    @app.get("/api/v1/arm")
    def get_arm(request: Request) -> JSONResponse:
        mode, config_version = arm.get_desired(request.app.state.conn)
        return JSONResponse(
            content={"mode": mode, "config_version": config_version}
        )

    @app.put("/api/v1/arm")
    def set_arm(payload: ArmRequest, request: Request) -> JSONResponse:
        mode, config_version = arm.set_desired(
            request.app.state.conn, payload.mode, time.time()
        )
        return JSONResponse(
            content={"mode": mode, "config_version": config_version}
        )

    @app.post("/api/v1/arm/ack")
    def arm_ack(payload: ArmAck, request: Request) -> JSONResponse:
        effective = arm.record_ack(
            request.app.state.conn, payload, time.time()
        )
        return JSONResponse(
            status_code=200, content={"status": "ok", "effective": effective}
        )

    @app.get("/api/v1/arm/status")
    def arm_status(request: Request) -> JSONResponse:
        return JSONResponse(content=arm.arm_status(request.app.state.conn))

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

    @app.post("/api/v1/events/{event_id}/close")
    def close_event_route(
        event_id: str, payload: EventClose, request: Request
    ) -> JSONResponse:
        close_event(request.app.state.conn, event_id, payload, time.time())
        return JSONResponse(
            status_code=200,
            content={"event_id": event_id, "status": "closed"},
        )

    @app.get("/api/v1/events")
    def query_events(
        request: Request,
        event_type: str | None = None,
        status: str | None = None,
        limit: int = 50,
        offset: int = 0,
    ) -> JSONResponse:
        items = list_events(
            request.app.state.conn,
            event_type=event_type,
            status=status,
            limit=limit,
            offset=offset,
        )
        return JSONResponse(
            content={"items": items, "limit": limit, "offset": offset}
        )

    @app.get("/api/v1/events/{event_id}")
    def event_detail(event_id: str, request: Request) -> JSONResponse:
        event = get_event(request.app.state.conn, event_id)
        if event is None:
            return JSONResponse(
                status_code=404, content={"detail": "event not found"}
            )
        return JSONResponse(content=event)

    return app
