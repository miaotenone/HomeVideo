from __future__ import annotations

import uuid

BASE = {
    "device_id": "v831-1",
    "event_type": "person_intrusion",
    "time_trusted": True,
    "model_version": "person-yolov2-1",
    "config_version": 3,
    "arm_mode": "away",
    "detections": [{"label": "person_intrusion", "confidence": 0.91,
                    "bbox": [10, 20, 80, 140], "zone": "door"}],
}


def _body(**over):
    body = dict(BASE)
    body["event_id"] = over.pop("event_id", str(uuid.uuid4()))
    body["started_at"] = over.pop("started_at", 1_700_000_000.0)
    body.update(over)
    return body


def test_create_event_returns_201_and_created_true(client):
    body = _body()
    r = client.post("/api/v1/events", json=body)
    assert r.status_code == 201
    assert r.json() == {"event_id": body["event_id"], "created": True}


def test_duplicate_create_is_idempotent(client):
    body = _body()
    assert client.post("/api/v1/events", json=body).status_code == 201
    r2 = client.post("/api/v1/events", json=body)
    assert r2.status_code == 200
    assert r2.json() == {"event_id": body["event_id"], "created": False}


def test_unknown_event_type_is_rejected(client):
    r = client.post("/api/v1/events", json=_body(event_type="alien"))
    assert r.status_code == 422


def test_event_id_traversal_is_rejected_and_writes_nothing(client, settings):
    # event_id is used verbatim as a filename stem everywhere (notifications,
    # snapshots, segments), so path separators, traversal and absolute paths
    # must be rejected at the protocol boundary before any file is touched.
    for bad in ("../escaped", "/tmp/pwned", "a/b", "..\\escaped", "x" * 129):
        r = client.post("/api/v1/events", json=_body(event_id=bad))
        assert r.status_code == 422, bad
    # No notification (or any other) file escaped the notification directory.
    assert list(settings.notification_dir.iterdir()) == []
    assert not (settings.data_dir / "escaped.json").exists()
    assert not (settings.data_dir.parent / "escaped.json").exists()
    # And no event row was written.
    assert client.app.state.conn.execute(
        "SELECT COUNT(*) AS c FROM events").fetchone()["c"] == 0


def test_untrusted_time_keeps_wall_clock_null(client):
    body = _body(time_trusted=False, started_at=None)
    assert client.post("/api/v1/events", json=body).status_code == 201
    row = client.app.state.conn.execute(
        "SELECT started_at, received_at, time_trusted FROM events WHERE event_id = ?",
        (body["event_id"],)).fetchone()
    assert bool(row["time_trusted"]) is False
    assert row["started_at"] is None          # 不可信时间不写入挂钟时间
    assert row["received_at"] is not None     # 集线器仍记录接收时刻用于排序