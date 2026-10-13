from __future__ import annotations

import hashlib
import uuid


def test_full_device_flow(client):
    device_id = "v831-1"
    eid = str(uuid.uuid4())

    # 1) 心跳
    assert client.post("/api/v1/device/heartbeat", json={
        "device_id": device_id, "boot_id": "boot-1", "uptime_ms": 5_000,
        "last_frame_ms": 4_900, "last_encode_ms": 4_800,
        "person_last_detection_ms": 4_700, "fire_last_detection_ms": 4_700,
        "smoke_last_detection_ms": 4_700, "video_ok": True, "recording_ok": True,
        "storage_free_bytes": 8_000_000, "arm_applied": "away",
        "arm_applied_version": 1, "time_trusted": True}).status_code == 200
    assert client.get(f"/api/v1/devices/{device_id}/health").json()["link"] == "online"

    # 2) 布防下发与确认
    client.put("/api/v1/arm", json={"mode": "away"})
    client.post("/api/v1/arm/ack", json={"device_id": device_id,
                                         "applied_mode": "away",
                                         "applied_version": 1})
    assert client.get("/api/v1/arm/status").json()["effective"] is True

    # 3) 事件、截图、片段、关闭
    client.post("/api/v1/events", json={
        "event_id": eid, "device_id": device_id, "event_type": "person_intrusion",
        "started_at": 1_700_000_000.0, "time_trusted": True,
        "model_version": "person-1", "config_version": 1, "arm_mode": "away",
        "detections": [{"label": "person_intrusion", "confidence": 0.93,
                        "bbox": [1, 2, 3, 4], "zone": "door"}]})
    client.post(f"/api/v1/events/{eid}/snapshot", content=b"jpegbytes",
                headers={"Content-Type": "image/jpeg"})
    for seq, data in enumerate((b"seg0", b"seg1", b"seg2")):
        client.put(f"/api/v1/events/{eid}/segments/{seq}", content=data,
                   headers={"X-Segment-Sha256": hashlib.sha256(data).hexdigest()})
    client.post(f"/api/v1/events/{eid}/close",
                json={"ended_at": 1_700_000_030.0, "duration_ms": 30_000,
                      "time_trusted": True})

    # 4) 断网重连后的幂等重传
    client.put(f"/api/v1/events/{eid}/segments/1", content=b"seg1",
               headers={"X-Segment-Sha256": hashlib.sha256(b"seg1").hexdigest()})
    client.post("/api/v1/events", json={
        "event_id": eid, "device_id": device_id, "event_type": "person_intrusion",
        "started_at": 1_700_000_000.0, "time_trusted": True,
        "model_version": "person-1", "config_version": 1, "arm_mode": "away",
        "detections": [{"label": "person_intrusion", "confidence": 0.93}]})

    # 5) 回溯
    detail = client.get(f"/api/v1/events/{eid}").json()
    assert detail["status"] == "closed"
    assert detail["snapshot"]["content_type"] == "image/jpeg"
    assert [s["seq"] for s in detail["segments"]] == [0, 1, 2]
    assert client.get("/api/v1/events").json()["items"][0]["event_id"] == eid
