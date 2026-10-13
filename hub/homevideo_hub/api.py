from __future__ import annotations

from fastapi import FastAPI

from .config import Settings, load_settings
from .db import connect, init_db


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
    return app
