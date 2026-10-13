from __future__ import annotations

from homevideo_hub.db import connect, init_db
from homevideo_hub.health import device_health, overview, record_report
from homevideo_hub.models import DeviceReport

NOW = 1_700_000_000.0


def _report(**over):
    base = dict(device_id="v831-1", boot_id="boot-a", uptime_ms=100_000,
                last_frame_ms=99_500, last_encode_ms=99_000,
                person_last_detection_ms=98_500, fire_last_detection_ms=98_500,
                smoke_last_detection_ms=98_500, video_ok=True, recording_ok=True,
                storage_free_bytes=8_000_000, arm_applied="away",
                arm_applied_version=2, time_trusted=True)
    base.update(over)
    return DeviceReport(**base)


def _conn(tmp_path):
    conn = connect(tmp_path / "hub.db")
    init_db(conn)
    return conn


def test_never_seen_device_is_unknown(tmp_path):
    conn = _conn(tmp_path)
    h = device_health(conn, "ghost", NOW, timeout_s=30, stale_s=5)
    # A device that has never reported is unknown across the board — unknown is
    # never treated as healthy.
    assert h == {
        "device_id": "ghost",
        "link": "unknown",
        "video": "unknown",
        "detection": {"person": "unknown", "fire": "unknown", "smoke": "unknown"},
        "recording": "unknown",
        "last_seen_at": None,
    }
    assert h["link"] == "unknown"
    assert h["video"] == "unknown"


def test_recent_report_is_online_with_ok_detectors(tmp_path):
    conn = _conn(tmp_path)
    record_report(conn, _report(), NOW)
    h = device_health(conn, "v831-1", NOW, timeout_s=30, stale_s=5)
    assert h["link"] == "online"
    assert h["video"] == "ok"
    assert h["detection"] == {"person": "ok", "fire": "ok", "smoke": "ok"}
    # Detection has no separate failure state: only ok/stale/unknown are ever
    # produced (检测失效 surfaces as "stale").
    assert set(h["detection"].values()) <= {"ok", "stale", "unknown"}


def test_device_is_offline_after_timeout(tmp_path):
    conn = _conn(tmp_path)
    record_report(conn, _report(), NOW)
    h = device_health(conn, "v831-1", NOW + 31, timeout_s=30, stale_s=5)
    assert h["link"] == "offline"


def test_stalled_detector_is_stale_not_ok(tmp_path):
    conn = _conn(tmp_path)
    # 火灾检测距上次完成已 40 秒，超过 5 秒陈旧阈值
    record_report(conn, _report(fire_last_detection_ms=100_000 - 40_000), NOW)
    h = device_health(conn, "v831-1", NOW, timeout_s=30, stale_s=5)
    assert h["detection"]["fire"] == "stale"
    assert h["detection"]["person"] == "ok"


def test_staleness_threshold_is_exclusive(tmp_path):
    conn = _conn(tmp_path)
    # age exactly equal to stale_s (5000 ms) is still ok...
    record_report(conn, _report(person_last_detection_ms=100_000 - 5_000), NOW)
    at_boundary = device_health(conn, "v831-1", NOW, timeout_s=30, stale_s=5)
    assert at_boundary["detection"]["person"] == "ok"
    # ...one millisecond more crosses into stale.
    record_report(conn, _report(person_last_detection_ms=100_000 - 5_001), NOW)
    past_boundary = device_health(conn, "v831-1", NOW, timeout_s=30, stale_s=5)
    assert past_boundary["detection"]["person"] == "stale"


def test_hardware_failure_reported_as_failed(tmp_path):
    conn = _conn(tmp_path)
    record_report(conn, _report(video_ok=False, recording_ok=False), NOW)
    h = device_health(conn, "v831-1", NOW, timeout_s=30, stale_s=5)
    assert h["video"] == "failed"
    assert h["recording"] == "failed"


def test_overview_includes_all_devices(tmp_path):
    conn = _conn(tmp_path)
    record_report(conn, _report(), NOW)
    assert [d["device_id"] for d in overview(conn, NOW, timeout_s=30, stale_s=5)] == ["v831-1"]


def test_overview_has_one_entry_per_known_device(tmp_path):
    conn = _conn(tmp_path)
    record_report(conn, _report(), NOW)
    record_report(conn, _report(device_id="v831-2"), NOW + 1)
    entries = overview(conn, NOW + 1, timeout_s=30, stale_s=5)
    assert [d["device_id"] for d in entries] == ["v831-1", "v831-2"]
    assert [d["link"] for d in entries] == ["online", "online"]


def test_heartbeat_endpoint_accepts_report(client):
    r = client.post("/api/v1/device/heartbeat", json=_report().model_dump())
    assert r.status_code == 200
    assert client.get("/api/v1/devices/v831-1/health").json()["link"] == "online"
