from __future__ import annotations

import dataclasses
import hashlib
import uuid

from homevideo_hub.api import create_app
from fastapi.testclient import TestClient

SEG = b"\x00\x00\x00\x01" + b"h264payload" * 32
SEG_SHA = hashlib.sha256(SEG).hexdigest()


def _event(client):
    eid = str(uuid.uuid4())
    client.post("/api/v1/events", json={
        "event_id": eid, "device_id": "v831-1", "event_type": "person_intrusion",
        "started_at": 1_700_000_000.0, "time_trusted": True,
        "model_version": "person-1", "config_version": 1, "arm_mode": "away",
        "detections": [{"label": "person_intrusion", "confidence": 0.9}]})
    return eid


def _put(client, eid, seq, data, sha):
    return client.put(f"/api/v1/events/{eid}/segments/{seq}", content=data,
                      headers={"X-Segment-Sha256": sha})


def test_segment_stored(client, settings):
    eid = _event(client)
    r = _put(client, eid, 0, SEG, SEG_SHA)
    assert r.status_code == 201
    assert r.json() == {"event_id": eid, "seq": 0, "status": "stored", "sha256": SEG_SHA}
    assert len(list(settings.segment_dir.iterdir())) == 1


def test_duplicate_segment_same_checksum_is_idempotent(client, settings):
    eid = _event(client)
    _put(client, eid, 0, SEG, SEG_SHA)
    r = _put(client, eid, 0, SEG, SEG_SHA)
    assert r.status_code == 200
    assert r.json()["status"] == "duplicate"
    assert len(list(settings.segment_dir.iterdir())) == 1


def test_same_seq_different_checksum_conflicts(client, settings):
    eid = _event(client)
    _put(client, eid, 0, SEG, SEG_SHA)
    r = _put(client, eid, 0, b"other-content", hashlib.sha256(b"other-content").hexdigest())
    assert r.status_code == 409
    assert len(list(settings.segment_dir.iterdir())) == 1


def test_declared_checksum_mismatch_returns_400(client, settings):
    eid = _event(client)
    r = _put(client, eid, 0, SEG, "0" * 64)
    assert r.status_code == 400
    assert not settings.segment_dir.exists() or list(settings.segment_dir.iterdir()) == []


def test_segment_before_metadata_returns_404_and_writes_nothing(client, settings):
    r = _put(client, str(uuid.uuid4()), 0, SEG, SEG_SHA)
    assert r.status_code == 404
    assert not settings.segment_dir.exists() or list(settings.segment_dir.iterdir()) == []


def test_insufficient_storage_returns_507(settings, monkeypatch):
    small = dataclasses.replace(settings, max_event_bytes=8)
    with TestClient(create_app(small)) as client:
        eid = _event(client)
        r = _put(client, eid, 0, SEG, SEG_SHA)
        assert r.status_code == 507
        assert not small.segment_dir.exists() or list(small.segment_dir.iterdir()) == []
