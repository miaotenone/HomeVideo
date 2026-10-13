from __future__ import annotations


def test_default_arm_is_unknown_and_not_effective(client):
    s = client.get("/api/v1/arm/status").json()
    assert s["desired"]["mode"] == "unknown"
    assert s["applied"] is None
    assert s["effective"] is False


def test_set_away_is_pending_until_device_acks(client):
    r = client.put("/api/v1/arm", json={"mode": "away"})
    assert r.status_code == 200
    assert r.json() == {"mode": "away", "config_version": 1}
    s = client.get("/api/v1/arm/status").json()
    assert s["desired"]["mode"] == "away"
    assert s["effective"] is False
    ack = client.post("/api/v1/arm/ack",
                      json={"device_id": "v831-1", "applied_mode": "away",
                            "applied_version": 1})
    assert ack.status_code == 200
    s = client.get("/api/v1/arm/status").json()
    assert s["effective"] is True
    assert s["applied"]["mode"] == "away"


def test_stale_ack_does_not_become_effective(client):
    client.put("/api/v1/arm", json={"mode": "home"})
    client.put("/api/v1/arm", json={"mode": "away"})
    client.post("/api/v1/arm/ack",
                json={"device_id": "v831-1", "applied_mode": "home",
                      "applied_version": 1})
    s = client.get("/api/v1/arm/status").json()
    assert s["desired"]["mode"] == "away"
    assert s["effective"] is False


def test_ack_before_any_config_is_not_effective(client):
    r = client.post("/api/v1/arm/ack",
                    json={"device_id": "v831-1", "applied_mode": "away",
                          "applied_version": 1})
    assert r.status_code == 200
    assert client.get("/api/v1/arm/status").json()["effective"] is False


def test_invalid_mode_is_rejected(client):
    assert client.put("/api/v1/arm", json={"mode": "vacation"}).status_code == 422


def test_device_reads_desired_config(client):
    client.put("/api/v1/arm", json={"mode": "away"})
    body = client.get("/api/v1/arm").json()
    assert body == {"mode": "away", "config_version": 1}
