from __future__ import annotations

import uuid


def _event(client, event_type="person_intrusion", **over):
    eid = str(uuid.uuid4())
    body = {"event_id": eid, "device_id": "v831-1", "event_type": event_type,
            "started_at": 1_700_000_000.0, "time_trusted": True,
            "model_version": "m1", "config_version": 1, "arm_mode": "away",
            "detections": [{"label": event_type, "confidence": 0.9}]}
    body.update(over)
    client.post("/api/v1/events", json=body)
    return eid


def test_close_event_marks_closed(client):
    eid = _event(client)
    r = client.post(f"/api/v1/events/{eid}/close",
                    json={"ended_at": 1_700_000_030.0, "duration_ms": 30000,
                          "time_trusted": True})
    assert r.status_code == 200
    assert r.json()["status"] == "closed"
    assert client.get(f"/api/v1/events/{eid}").json()["status"] == "closed"


def test_close_before_any_segment_is_allowed(client):
    eid = _event(client)
    r = client.post(f"/api/v1/events/{eid}/close",
                    json={"ended_at": None, "duration_ms": 1000, "time_trusted": False})
    assert r.status_code == 200
    assert client.get(f"/api/v1/events/{eid}").json()["segments"] == []


def test_detail_orders_segments_by_seq(client):
    eid = _event(client)
    for seq, data in ((2, b"two"), (0, b"zero"), (1, b"one")):
        import hashlib
        client.put(f"/api/v1/events/{eid}/segments/{seq}", content=data,
                   headers={"X-Segment-Sha256": hashlib.sha256(data).hexdigest()})
    segs = client.get(f"/api/v1/events/{eid}").json()["segments"]
    assert [s["seq"] for s in segs] == [0, 1, 2]


def test_list_filters_by_type_and_status(client):
    person = _event(client, "person_intrusion")
    _event(client, "fire_suspected")
    client.post(f"/api/v1/events/{person}/close",
                json={"ended_at": None, "duration_ms": 1, "time_trusted": False})
    only_fire = client.get("/api/v1/events", params={"event_type": "fire_suspected"}).json()
    assert {e["event_type"] for e in only_fire["items"]} == {"fire_suspected"}
    closed = client.get("/api/v1/events", params={"status": "closed"}).json()
    assert [e["event_id"] for e in closed["items"]] == [person]


def test_unknown_event_detail_returns_404(client):
    assert client.get(f"/api/v1/events/{uuid.uuid4()}").status_code == 404


def test_limit_and_offset_are_bounded(client):
    # Unbounded paging would let a caller dump the whole table (limit=-1);
    # the boundary now rejects out-of-range values with 422.
    assert client.get("/api/v1/events", params={"limit": 0}).status_code == 422
    assert client.get("/api/v1/events", params={"limit": 501}).status_code == 422
    assert client.get("/api/v1/events", params={"limit": -1}).status_code == 422
    assert client.get("/api/v1/events", params={"offset": -1}).status_code == 422
    ok = client.get("/api/v1/events", params={"limit": 500, "offset": 0})
    assert ok.status_code == 200
    assert ok.json()["limit"] == 500


def test_untrusted_close_keeps_duration_and_null_times(client):
    # Fallback ordering for an untrusted clock: boot id + monotonic start
    # survive, and the event length must survive as duration_ms.
    eid = _event(client, time_trusted=False, started_at=None)
    client.post(f"/api/v1/events/{eid}/close",
                json={"ended_at": 1_700_000_030.0, "duration_ms": 42_000,
                      "time_trusted": False})
    detail = client.get(f"/api/v1/events/{eid}").json()
    assert detail["started_at"] is None
    assert detail["ended_at"] is None          # untrusted end time not persisted
    assert detail["duration_ms"] == 42_000     # duration always persisted

    listed = [e for e in client.get("/api/v1/events").json()["items"]
              if e["event_id"] == eid][0]
    assert listed["started_at"] is None
    assert listed["ended_at"] is None
    assert listed["duration_ms"] == 42_000


def test_trusted_close_persists_ended_at(client):
    eid = _event(client, time_trusted=True, started_at=1_700_000_000.0)
    client.post(f"/api/v1/events/{eid}/close",
                json={"ended_at": 1_700_000_030.0, "duration_ms": 30_000,
                      "time_trusted": True})
    detail = client.get(f"/api/v1/events/{eid}").json()
    assert detail["started_at"] == 1_700_000_000.0
    assert detail["ended_at"] == 1_700_000_030.0
    assert detail["duration_ms"] == 30_000
