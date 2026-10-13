from __future__ import annotations

import hashlib
import time
import uuid

from homevideo_hub.retention import run_cleanup

DAY = 86_400.0


def _make_event(client):
    eid = str(uuid.uuid4())
    client.post("/api/v1/events", json={
        "event_id": eid, "device_id": "v831-1", "event_type": "person_intrusion",
        "started_at": None, "time_trusted": False,
        "model_version": "m1", "config_version": 1, "arm_mode": "away",
        "detections": [{"label": "person_intrusion", "confidence": 0.9}]})
    client.post(f"/api/v1/events/{eid}/snapshot",
                content=b"\x89PNG\r\n\x1a\n" + b"\x00" * 16,
                headers={"Content-Type": "image/png"})
    data = b"payload" * 8
    client.put(f"/api/v1/events/{eid}/segments/0", content=data,
               headers={"X-Segment-Sha256": hashlib.sha256(data).hexdigest()})
    client.post(f"/api/v1/events/{eid}/close",
                json={"ended_at": None, "duration_ms": 1_000, "time_trusted": False})
    return eid


def _age(conn, event_id, seconds):
    conn.execute("UPDATE events SET received_at = received_at - ? WHERE event_id = ?",
                 (seconds, event_id))
    conn.commit()


def test_closed_event_older_than_retention_is_deleted(client, settings):
    eid = _make_event(client)
    conn = client.app.state.conn
    # The event starts with both media kinds (one snapshot, one segment).
    assert list(settings.snapshot_dir.glob(f"{eid}*")) != []
    assert list(settings.segment_dir.glob(f"{eid}*")) != []
    _age(conn, eid, 8 * DAY)
    result = run_cleanup(conn, settings, time.time())
    assert result["deleted_events"] == 1
    assert result["bytes_freed"] > 0
    assert client.get(f"/api/v1/events/{eid}").status_code == 404
    # Both media files are removed from disk...
    assert list(settings.segment_dir.glob(f"{eid}*")) == []
    assert list(settings.snapshot_dir.glob(f"{eid}*")) == []
    # ...the child rows are gone (FK cascade)...
    assert conn.execute(
        "SELECT COUNT(*) AS c FROM segments WHERE event_id = ?", (eid,)
    ).fetchone()["c"] == 0
    assert conn.execute(
        "SELECT COUNT(*) AS c FROM snapshots WHERE event_id = ?", (eid,)
    ).fetchone()["c"] == 0
    # ...and the deletion is recorded with a nonzero bytes_freed.
    deletion = conn.execute(
        "SELECT event_id, bytes_freed FROM deletions WHERE event_id = ?", (eid,)
    ).fetchone()
    assert deletion is not None
    assert deletion["bytes_freed"] > 0


def test_recent_event_is_kept(client, settings):
    eid = _make_event(client)
    assert run_cleanup(client.app.state.conn, settings, time.time())["deleted_events"] == 0
    assert client.get(f"/api/v1/events/{eid}").status_code == 200


def test_active_event_is_never_deleted(client, settings):
    eid = str(uuid.uuid4())
    client.post("/api/v1/events", json={
        "event_id": eid, "device_id": "v831-1", "event_type": "person_intrusion",
        "started_at": None, "time_trusted": False, "model_version": "m1",
        "config_version": 1, "arm_mode": "away",
        "detections": [{"label": "person_intrusion", "confidence": 0.9}]})
    conn = client.app.state.conn
    _age(conn, eid, 30 * DAY)
    assert run_cleanup(conn, settings, time.time())["deleted_events"] == 0


def test_cleanup_endpoint_returns_counts(client):
    r = client.post("/api/v1/maintenance/cleanup")
    assert r.status_code == 200
    assert set(r.json()) == {"deleted_events", "bytes_freed"}
