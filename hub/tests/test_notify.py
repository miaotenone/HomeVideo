from __future__ import annotations

import dataclasses
import json
import uuid

from fastapi.testclient import TestClient

from homevideo_hub.api import create_app
from homevideo_hub.config import load_settings


def _body(eid=None):
    return {"event_id": eid or str(uuid.uuid4()), "device_id": "v831-1",
            "event_type": "person_intrusion", "started_at": 1_700_000_000.0,
            "time_trusted": True, "model_version": "person-1",
            "config_version": 1, "arm_mode": "away",
            "detections": [{"label": "person_intrusion", "confidence": 0.9}]}


def test_new_event_writes_one_notification(client, settings):
    body = _body()
    client.post("/api/v1/events", json=body)
    files = list(settings.notification_dir.iterdir())
    assert len(files) == 1
    payload = json.loads(files[0].read_text())
    assert payload["event_id"] == body["event_id"]
    assert payload["event_type"] == "person_intrusion"


def test_duplicate_event_does_not_notify_again(client, settings):
    body = _body()
    client.post("/api/v1/events", json=body)
    client.post("/api/v1/events", json=body)
    assert len(list(settings.notification_dir.iterdir())) == 1


def test_notifier_failure_does_not_fail_ingest(tmp_path):
    settings = dataclasses.replace(load_settings(data_dir=tmp_path), notifier="boom")
    with TestClient(create_app(settings)) as client:
        body = _body()
        r = client.post("/api/v1/events", json=body)
        assert r.status_code == 201
        assert client.get(f"/api/v1/events/{body['event_id']}").status_code == 200
