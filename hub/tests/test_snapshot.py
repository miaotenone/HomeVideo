from __future__ import annotations

import dataclasses
import hashlib
import uuid

from fastapi.testclient import TestClient

from homevideo_hub.api import create_app

PNG = b"\x89PNG\r\n\x1a\n" + b"\x00" * 64


def _event(client, event_id=None, **over):
    body = {
        "event_id": event_id or str(uuid.uuid4()),
        "device_id": "v831-1", "event_type": "fire_suspected",
        "started_at": 1_700_000_000.0, "time_trusted": True,
        "model_version": "fire-1", "config_version": 1, "arm_mode": "away",
        "detections": [{"label": "fire_suspected", "confidence": 0.8}],
    }
    body.update(over)
    client.post("/api/v1/events", json=body)
    return body["event_id"]


def test_snapshot_stored_on_disk(client, settings):
    eid = _event(client)
    r = client.post(f"/api/v1/events/{eid}/snapshot", content=PNG,
                    headers={"Content-Type": "image/png"})
    assert r.status_code == 201
    assert r.json() == {"event_id": eid, "status": "stored"}
    files = list(settings.snapshot_dir.iterdir())
    assert len(files) == 1
    assert files[0].read_bytes() == PNG
    assert hashlib.sha256(PNG).hexdigest() == hashlib.sha256(
        files[0].read_bytes()).hexdigest()


def test_duplicate_snapshot_is_idempotent(client, settings):
    eid = _event(client)
    client.post(f"/api/v1/events/{eid}/snapshot", content=PNG,
                headers={"Content-Type": "image/png"})
    r = client.post(f"/api/v1/events/{eid}/snapshot", content=PNG,
                    headers={"Content-Type": "image/png"})
    assert r.status_code == 200
    assert r.json()["status"] == "duplicate"
    assert len(list(settings.snapshot_dir.iterdir())) == 1


def test_snapshot_for_unknown_event_returns_404_and_writes_nothing(client, settings):
    r = client.post(f"/api/v1/events/{uuid.uuid4()}/snapshot", content=PNG,
                    headers={"Content-Type": "image/png"})
    assert r.status_code == 404
    assert not settings.snapshot_dir.exists() or list(settings.snapshot_dir.iterdir()) == []


def test_snapshot_over_max_event_bytes_returns_507_and_keeps_nothing(settings):
    # The per-event cumulative cap applies to snapshots exactly as it does to
    # segments: an oversized snapshot is rejected with 507 and nothing is
    # published (no file, no row).
    small = dataclasses.replace(settings, max_event_bytes=8)
    with TestClient(create_app(small)) as client:
        eid = _event(client)
        r = client.post(f"/api/v1/events/{eid}/snapshot", content=PNG,
                        headers={"Content-Type": "image/png"})
        assert r.status_code == 507
        assert not small.snapshot_dir.exists() or list(small.snapshot_dir.iterdir()) == []
        assert client.app.state.conn.execute(
            "SELECT COUNT(*) AS c FROM snapshots WHERE event_id = ?", (eid,)
        ).fetchone()["c"] == 0

