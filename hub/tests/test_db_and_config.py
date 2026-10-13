from __future__ import annotations

from homevideo_hub.config import load_settings
from homevideo_hub.db import connect, init_db

EXPECTED_TABLES = {
    "meta", "devices", "events", "snapshots",
    "segments", "arm_config", "arm_ack", "deletions",
}


def test_init_db_creates_tables(tmp_path):
    conn = connect(tmp_path / "hub.db")
    init_db(conn)
    names = {r["name"] for r in conn.execute(
        "SELECT name FROM sqlite_master WHERE type='table'")}
    assert EXPECTED_TABLES <= names


def test_init_db_is_idempotent(tmp_path):
    conn = connect(tmp_path / "hub.db")
    init_db(conn)
    init_db(conn)
    assert conn.execute("SELECT COUNT(*) AS c FROM meta").fetchone()["c"] >= 1


def test_settings_defaults(tmp_path):
    s = load_settings(data_dir=tmp_path)
    assert s.retention_days == 7
    assert s.max_event_bytes == 2_000_000_000
    assert s.metadata_reserve_bytes == 64 * 1024 * 1024
    assert s.heartbeat_timeout_seconds == 30
    assert s.detection_stale_seconds == 5
    assert s.notifier == "file"
    assert s.snapshot_dir == tmp_path / "media" / "snapshots"
    assert s.segment_dir == tmp_path / "media" / "segments"


def test_settings_env_override(tmp_path):
    s = load_settings(data_dir=tmp_path, env={"HOMEVIDEO_RETENTION_DAYS": "30",
                                             "HOMEVIDEO_NOTIFIER": "file"})
    assert s.retention_days == 30
