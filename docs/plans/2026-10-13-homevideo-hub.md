# HomeVideo 集线器（树莓派）服务 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在树莓派上实现轻量事件集线器，接收 V831 的事件、截图与视频片段，幂等落盘，并提供事件回溯、布防配置、设备健康与通知接口。

**Architecture:** 单个 FastAPI 应用（应用工厂 + 依赖注入），HTTP 层与业务逻辑分离。SQLite 存元数据，文件系统存媒体文件；所有写入先落盘并提交数据库，再向设备确认。设备协议由 Pydantic 模型固定，测试使用模拟设备驱动真实 HTTP 接口。

**Tech Stack:** Python 3.11+、FastAPI、Pydantic v2、uvicorn、SQLite（标准库 `sqlite3`）、pytest + FastAPI TestClient（httpx）。

**Spec:** `docs/homevideo-design.md`

## Global Constraints

以下取自方案，逐字复制；每个任务都隐含包含本节。

- 事件类型固定为三值：`person_intrusion`、`fire_suspected`、`smoke_suspected`。
- 布防模式固定为三值：`home`、`away`、`unknown`；`unknown` 表示未初始化，不得显示为已布防或已生效。
- 事件信息字段至少包含：设备 ID、事件 ID、事件类型、开始与结束时间、时间可信状态、模型版本、配置版本、布防状态、检测结果及截图、视频片段列表与上传状态。
- 事件前缓冲 10 秒、结束后补录 20 秒；长事件按约 60 秒分段，归属同一事件。（设备侧行为；集线器只保存并表达，不生成片段。）
- 集线器必须在持久保存后才能确认上传；确认后设备才可清理。
- 所有事件与片段操作必须幂等；片段以校验值判定重复，序号冲突且校验值不同为错误。
- 上传必须流式写入，不得把完整录像读入内存。
- 集线器不常驻解码、转码或推理；只接收、存储与转发。
- 事件保留天数与总容量可配置；默认值（保留 7 天、单事件 2,000,000,000 字节上限、元数据预留 64 MiB、心跳超时 30 秒、检测陈旧阈值 5 秒）为临时默认值，必须可配置。
- 验收参考目标（用于验证阶段，不在本子系统断言）：网络正常时板端确认事件后 2 秒内集线器收到事件信息；手机观看首帧约 5 秒、延迟约 3 秒。
- 依赖上限：Python 3.11+；FastAPI 与 Pydantic v2；不引入数据库服务、消息队列或转码组件。

## 范围说明

方案 `docs/homevideo-design.md` 覆盖多个子系统。按验证独立性拆分，每个子系统单独成计划：

- **子系统 A：树莓派集线器（本计划）。** 纯软件，无需硬件即可测试，并固定设备必须满足的协议契约。本计划覆盖事件接收、存储、回溯、布防、健康与通知；**实时视频转发（go2rtc 接入、Tailscale 组网、HTTPS 入口）不在本计划内**，它依赖 V831 已能输出压缩流，见“后续计划”。
- **子系统 B：V831 设备软件。** 依赖方案 §8 阶段 A 的固件、SDK、编码接口与模型转换实测结果，须待 A 阶段发现完成后再制定。
- **子系统 C：手机网页界面。** 在 A 提供稳定 API 后单独制定。
- **子系统 D：板卡能力验证脚本与记录表。** 依赖实机，单独制定。

本计划只实现子系统 A 的事件与配置部分。

## 文件结构

```
hub/
  pyproject.toml                  # 包与依赖声明
  homevideo_hub/
    __init__.py
    config.py                     # Settings 与 load_settings
    db.py                         # 连接、建表、meta
    models.py                     # 协议 Pydantic 模型
    storage.py                    # 流式写入、校验、容量
    events.py                     # 事件/截图/片段/查询的业务逻辑
    arm.py                        # 布防配置与确认
    health.py                     # 设备报告与派生的健康状态
    notify.py                     # 通知接口与文件实现
    retention.py                  # 保留期与容量清理
    api.py                        # FastAPI 应用工厂与路由
  tests/
    conftest.py
    test_db_and_config.py
    test_event_ingest.py
    test_snapshot.py
    test_segments.py
    test_event_queries.py
    test_health.py
    test_arm.py
    test_notify.py
    test_retention.py
    test_end_to_end.py
```

每个模块单一职责：`models.py` 只定义协议；`events.py` 只做事件领域逻辑；`api.py` 只做 HTTP 映射与流式请求处理。

## Review Focus

方案隐含、但没有任何任务测试直接覆盖、最可能出问题的五类输入。每类都在其归属任务中加了对应测试：

1. **重试的重复上传**：网络抖动后设备重传同一 `event_id`、同一 `seq`，校验值相同与不同两种情形。
2. **乱序到达**：片段先于事件元数据、关闭先于片段、设备确认先于布防配置下发。
3. **容量耗尽**：上传中途磁盘或配额不足，必须拒绝而不是写入半截文件。
4. **时钟不可信**：设备时间未同步、时间跳变、事件跨越日期边界时的排序与保留。
5. **未初始化状态**：从未上报的设备、未设置的布防配置，必须读为 `unknown`，不得当作在线、健康或已布防。

---

### Task 1: 项目骨架、配置与数据库

**Files:**
- Create: `hub/pyproject.toml`
- Create: `hub/homevideo_hub/__init__.py`
- Create: `hub/homevideo_hub/config.py`
- Create: `hub/homevideo_hub/db.py`
- Test: `hub/tests/test_db_and_config.py`
- Test: `hub/tests/conftest.py`

**Interfaces:**
- Consumes: 无
- Produces:
  - `homevideo_hub.config.Settings`：冻结 dataclass，字段 `data_dir: Path`、`db_path: Path`、`media_dir: Path`、`snapshot_dir: Path`、`segment_dir: Path`、`notification_dir: Path`、`retention_days: int`、`max_event_bytes: int`、`metadata_reserve_bytes: int`、`heartbeat_timeout_seconds: int`、`detection_stale_seconds: int`、`notifier: str`
  - `homevideo_hub.config.load_settings(data_dir: Path | None = None, env: Mapping[str, str] | None = None) -> Settings`
  - `homevideo_hub.db.connect(db_path: Path) -> sqlite3.Connection`（`row_factory = sqlite3.Row`，`PRAGMA journal_mode=WAL`、`PRAGMA foreign_keys=ON`）
  - `homevideo_hub.db.init_db(conn: sqlite3.Connection) -> None`
  - `homevideo_hub.api.create_app(settings: Settings | None = None) -> FastAPI`，把连接与配置存到 `app.state.conn`、`app.state.settings`（在工厂内直接赋值，不依赖启动事件），供后续任务与测试使用
  - 数据表：`meta(key TEXT PRIMARY KEY, value TEXT)`、`devices`、`events`、`snapshots`、`segments`、`arm_config`、`arm_ack`、`deletions`

- [ ] **Step 1: 写失败测试**

`hub/tests/conftest.py`：

```python
from __future__ import annotations

import pytest
from fastapi.testclient import TestClient

from homevideo_hub.api import create_app
from homevideo_hub.config import load_settings


@pytest.fixture
def settings(tmp_path):
    return load_settings(data_dir=tmp_path)


@pytest.fixture
def client(settings):
    with TestClient(create_app(settings)) as c:
        yield c
```

`hub/tests/test_db_and_config.py`：

```python
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
```

- [ ] **Step 2: 运行测试确认失败**

Run: `python -m pytest hub/tests/test_db_and_config.py -q`
Expected: FAIL，`ModuleNotFoundError: No module named 'homevideo_hub'`

- [ ] **Step 3: 实现 `hub/pyproject.toml`、`config.py`、`db.py`、`api.py` 骨架**

`hub/pyproject.toml`：包名 `homevideo-hub`，`requires-python = ">=3.11"`，依赖 `fastapi`、`uvicorn[standard]`、`pydantic>=2`，可选依赖组 `dev = ["pytest", "httpx"]`，构建后端 setuptools。

`config.py`：`Settings` 为 `@dataclass(frozen=True)`。`load_settings` 从 `env`（默认 `os.environ`）读取 `HOMEVIDEO_DATA_DIR`、`HOMEVIDEO_RETENTION_DAYS`、`HOMEVIDEO_MAX_EVENT_BYTES`、`HOMEVIDEO_METADATA_RESERVE_BYTES`、`HOMEVIDEO_HEARTBEAT_TIMEOUT_SECONDS`、`HOMEVIDEO_DETECTION_STALE_SECONDS`、`HOMEVIDEO_NOTIFIER` 覆盖默认值，整数字段用 `int()` 转换。`media_dir = data_dir / "media"`，`snapshot_dir = media_dir / "snapshots"`，`segment_dir = media_dir / "segments"`，`notification_dir = data_dir / "notifications"`，`db_path = data_dir / "hub.db"`。

`db.py`：`connect` 设置 `row_factory`、WAL、外键；`init_db` 在单个事务中 `CREATE TABLE IF NOT EXISTS` 全部表并写入 `meta('schema_version','1')`。

`api.py`：先只提供 `create_app(settings: Settings | None = None) -> FastAPI`，内部 `init_db` 建表并创建目录，返回应用实例，无业务路由。

- [ ] **Step 4: 运行测试确认通过**

Run: `python -m pytest hub/tests -q`
Expected: PASS（conftest 可导入，配置与建表测试全部通过）

- [ ] **Step 5: 提交**

```bash
git add hub/pyproject.toml hub/homevideo_hub/ hub/tests/
git commit -m "feat(hub): scaffold config, sqlite schema and app factory"
```

---

### Task 2: 事件元数据接收（幂等）

**Files:**
- Create: `hub/homevideo_hub/models.py`
- Create: `hub/homevideo_hub/events.py`
- Modify: `hub/homevideo_hub/api.py`
- Test: `hub/tests/test_event_ingest.py`

**Interfaces:**
- Consumes: `Settings`、`connect`、`init_db`
- Produces:
  - `models.py`：`EventType = Literal["person_intrusion","fire_suspected","smoke_suspected"]`、`ArmMode = Literal["home","away","unknown"]`、`Detection`、`EventCreate`、`EventClose`、`DeviceReport`、`ArmRequest`、`ArmAck`、`AckResponse`
  - `EventCreate` 字段：`event_id: str`、`device_id: str`、`event_type: EventType`、`started_at: float | None`、`time_trusted: bool`、`device_boot_id: str | None`、`device_started_ms: int | None`、`model_version: str`、`config_version: int`、`arm_mode: ArmMode`、`detections: list[Detection]`
  - `events.upsert_event(conn, payload: EventCreate, now: float) -> tuple[str, bool]`，返回 `(event_id, created)`
  - 路由 `POST /api/v1/events`：新建返回 201 且 `{"event_id","created": true}`；已存在返回 200 且 `created: false`

- [ ] **Step 1: 写失败测试**

`hub/tests/test_event_ingest.py`：

```python
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


def test_untrusted_time_keeps_wall_clock_null(client):
    body = _body(time_trusted=False, started_at=None)
    assert client.post("/api/v1/events", json=body).status_code == 201
    row = client.app.state.conn.execute(
        "SELECT started_at, received_at, time_trusted FROM events WHERE event_id = ?",
        (body["event_id"],)).fetchone()
    assert bool(row["time_trusted"]) is False
    assert row["started_at"] is None          # 不可信时间不写入挂钟时间
    assert row["received_at"] is not None     # 集线器仍记录接收时刻用于排序
```

- [ ] **Step 2: 运行测试确认失败**

Run: `python -m pytest hub/tests/test_event_ingest.py -q`
Expected: FAIL，`404 Not Found`（路由不存在）

- [ ] **Step 3: 实现 `models.py`、`events.upsert_event` 与 `POST /api/v1/events`**

`events.upsert_event` 使用 `INSERT ... ON CONFLICT(event_id) DO UPDATE SET ...`，用 `cursor.rowcount` 与先查询区分是否新建；`created` 为首次插入。`started_at` 仅在 `time_trusted` 为真时保存，否则写 `NULL`。`received_at` 由集线器写入 `now`。`detections` 序列化为 JSON 存入 `events.detections_json`。

`POST /api/v1/events` 解析 `EventCreate`，调用 `upsert_event`，按 `created` 返回 201 或 200。

- [ ] **Step 4: 运行测试确认通过**

Run: `python -m pytest hub/tests/test_event_ingest.py -q`
Expected: PASS

- [ ] **Step 5: 提交**

```bash
git add hub/homevideo_hub/models.py hub/homevideo_hub/events.py hub/homevideo_hub/api.py hub/tests/test_event_ingest.py
git commit -m "feat(hub): idempotent event metadata ingest"
```

---

### Task 3: 截图上传

**Files:**
- Create: `hub/homevideo_hub/storage.py`
- Modify: `hub/homevideo_hub/events.py`
- Modify: `hub/homevideo_hub/api.py`
- Test: `hub/tests/test_snapshot.py`

**Interfaces:**
- Consumes: `events.upsert_event`、`Settings.snapshot_dir`
- Produces:
  - `storage.write_stream(stream: AsyncIterable[bytes], dest: Path) -> tuple[int, str]`（`async def`；`request.stream()` 是异步生成器，调用方必须 `await`），返回 `(size_bytes, sha256_hex)`，边写边算校验值
  - `storage.sha256_file(path: Path) -> str`
  - `storage.safe_delete(path: Path) -> None`
  - `events.store_snapshot(conn, event_id: str, dest: Path, sha256: str, size_bytes: int, content_type: str, now: float) -> str`，返回 `"stored"` 或 `"duplicate"`
  - 路由 `POST /api/v1/events/{event_id}/snapshot`：成功 201 `{"event_id","status":"stored"}`；事件不存在 404；重复 200 `status: "duplicate"`

- [ ] **Step 1: 写失败测试**

`hub/tests/test_snapshot.py`：

```python
from __future__ import annotations

import hashlib
import uuid

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
```

- [ ] **Step 2: 运行测试确认失败**

Run: `python -m pytest hub/tests/test_snapshot.py -q`
Expected: FAIL，`404`（路由不存在）

- [ ] **Step 3: 实现 `storage.py`、`events.store_snapshot` 与快照路由**

先校验事件存在，再写入临时文件并原子替换，最后写 `snapshots` 行；重复到达时若 `snapshots.event_id` 已存在且校验值一致，返回 `"duplicate"` 并删除临时文件。截图文件名固定为 `f"{event_id}{ext}"`，`ext` 由 `Content-Type` 推导（`image/png`→`.png`，`image/jpeg`→`.jpg`，其余→`.bin`）。路由用 `request.stream()` 读取请求体，不在内存中拼装完整图片。

- [ ] **Step 4: 运行测试确认通过**

Run: `python -m pytest hub/tests/test_snapshot.py -q`
Expected: PASS

- [ ] **Step 5: 提交**

```bash
git add hub/homevideo_hub/storage.py hub/homevideo_hub/events.py hub/homevideo_hub/api.py hub/tests/test_snapshot.py
git commit -m "feat(hub): streaming snapshot upload with idempotency"
```

---

### Task 4: 视频片段上传（流式、校验、容量）

**Files:**
- Modify: `hub/homevideo_hub/storage.py`
- Modify: `hub/homevideo_hub/events.py`
- Modify: `hub/homevideo_hub/api.py`
- Test: `hub/tests/test_segments.py`

**Interfaces:**
- Consumes: `storage.write_stream`（异步，需 `await`）、`events.get_segment`、`Settings.segment_dir`、`Settings.max_event_bytes`、`Settings.metadata_reserve_bytes`
- Produces:
  - `storage.InsufficientStorage(Exception)`
  - `storage.free_bytes(path: Path) -> int`
  - `storage.ensure_capacity(path: Path, required_bytes: int, reserve_bytes: int) -> None`，不足时抛 `InsufficientStorage`
  - `events.store_segment(conn, event_id: str, seq: int, dest: Path, sha256: str, size_bytes: int, now: float) -> str`，返回 `"stored"` 或 `"duplicate"`
  - `events.get_segment(conn, event_id: str, seq: int) -> sqlite3.Row | None`
  - 路由 `PUT /api/v1/events/{event_id}/segments/{seq}`，请求头 `X-Segment-Sha256`，响应 201 `{"event_id","seq","status":"stored","sha256"}`、200 `"duplicate"`、400 校验不符、404 事件不存在、409 同序号不同校验值、507 容量不足

- [ ] **Step 1: 写失败测试**

`hub/tests/test_segments.py`：

```python
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
```

- [ ] **Step 2: 运行测试确认失败**

Run: `python -m pytest hub/tests/test_segments.py -q`
Expected: FAIL，`404`（路由不存在）

- [ ] **Step 3: 实现容量检查、`events.store_segment` 与片段路由**

`ensure_capacity` 只检查磁盘剩余空间（`free_bytes(path) - reserve_bytes`）。单事件字节上限 `max_event_bytes` 在 `events.store_segment` 中按该事件已有片段与截图字节之和加本次所需判断，超出时抛 `InsufficientStorage`；测试通过把 `max_event_bytes` 设小来触发，无需伪造磁盘。片段文件名固定为 `f"{event_id}_{seq}.bin"`，写入临时文件后原子替换。校验值由流式计算得到，与 `X-Segment-Sha256` 比对。序号冲突且校验值不同返回 409。所有失败路径删除临时文件。

- [ ] **Step 4: 运行测试确认通过**

Run: `python -m pytest hub/tests/test_segments.py -q`
Expected: PASS

- [ ] **Step 5: 提交**

```bash
git add hub/homevideo_hub/storage.py hub/homevideo_hub/events.py hub/homevideo_hub/api.py hub/tests/test_segments.py
git commit -m "feat(hub): streaming segment upload with checksum and capacity guard"
```

---

### Task 5: 事件关闭与查询

**Files:**
- Modify: `hub/homevideo_hub/events.py`
- Modify: `hub/homevideo_hub/api.py`
- Test: `hub/tests/test_event_queries.py`

**Interfaces:**
- Consumes: `events.upsert_event`、`events.store_segment`、`models.EventClose`
- Produces:
  - `events.close_event(conn, event_id: str, payload: EventClose, now: float) -> None`，设置 `status='closed'` 与 `closed_at`，并持久化结束信息：`events` 表需增加 `ended_at REAL` 与 `duration_ms INTEGER` 两列（本任务可在 `db.py` 的建表语句中补上）。`ended_at` 仅在 `payload.time_trusted` 为真时写入，否则为 `NULL`；`duration_ms` 始终写入，它是设备时间不可信时还原事件时长的唯一依据
  - `events.get_event(conn, event_id: str) -> dict | None`，字段均为可 JSON 序列化的原生类型，且必须包含 `ended_at` 与 `duration_ms`；`snapshot` 为 `dict | None`，`segments` 为按 `seq` 升序的 `list[dict]`（不要把 `sqlite3.Row` 直接交给响应层）。`list_events` 返回的每项同样包含这两个字段
  - `events.list_events(conn, *, event_type=None, status=None, limit=50, offset=0) -> list[dict]`，按 `received_at DESC` 排序
  - 路由 `POST /api/v1/events/{event_id}/close`、`GET /api/v1/events`、`GET /api/v1/events/{event_id}`

- [ ] **Step 1: 写失败测试**

`hub/tests/test_event_queries.py`：

```python
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
```

- [ ] **Step 2: 运行测试确认失败**

Run: `python -m pytest hub/tests/test_event_queries.py -q`
Expected: FAIL，查询路由 `404`

- [ ] **Step 3: 实现关闭、详情与列表**

`list_events` 返回 `{"items": [...], "limit": ..., "offset": ...}`。事件在未关闭时为 `status='active'`。时间不可信的事件同样按 `received_at` 排序，`started_at` 为 `None`。

关闭流程必须保留设备给出的结束信息：在 `events` 表增加 `ended_at REAL` 与 `duration_ms INTEGER`，`close_event` 在 `payload.time_trusted` 为真时写入 `ended_at`，并始终写入 `duration_ms`。时间不可信的事件 `started_at` 与 `ended_at` 都为 `NULL`，此时 `duration_ms` 是唯一的时长来源（对应方案“用启动标识、单调时钟及事件时长保存记录”）。详情与列表都要返回这两个字段。

- [ ] **Step 4: 运行测试确认通过**

Run: `python -m pytest hub/tests/test_event_queries.py -q`
Expected: PASS

- [ ] **Step 5: 提交**

```bash
git add hub/homevideo_hub/events.py hub/homevideo_hub/api.py hub/tests/test_event_queries.py
git commit -m "feat(hub): event close and query API"
```

---

### Task 6: 设备心跳与健康状态

**Files:**
- Create: `hub/homevideo_hub/health.py`
- Modify: `hub/homevideo_hub/api.py`
- Test: `hub/tests/test_health.py`

**Interfaces:**
- Consumes: `models.DeviceReport`、`Settings.heartbeat_timeout_seconds`、`Settings.detection_stale_seconds`
- Produces:
  - `health.record_report(conn, report: DeviceReport, now: float) -> None`（写 `devices` 表）
  - `health.device_health(conn, device_id: str, now: float, *, timeout_s: int, stale_s: int) -> dict`
  - `health.overview(conn, now: float, *, timeout_s: int, stale_s: int) -> list[dict]`
  - 健康字典字段：`device_id`、`link`（`online|offline|unknown`）、`video`、`detection`（`person|fire|smoke` 各自 `ok|stale|failed|unknown`）、`recording`、`last_seen_at`
  - 路由 `POST /api/v1/device/heartbeat`、`GET /api/v1/devices/{device_id}/health`、`GET /api/v1/health`

- [ ] **Step 1: 写失败测试**

`hub/tests/test_health.py`：

```python
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
    assert h["link"] == "unknown"
    assert h["video"] == "unknown"


def test_recent_report_is_online_with_ok_detectors(tmp_path):
    conn = _conn(tmp_path)
    record_report(conn, _report(), NOW)
    h = device_health(conn, "v831-1", NOW, timeout_s=30, stale_s=5)
    assert h["link"] == "online"
    assert h["video"] == "ok"
    assert h["detection"] == {"person": "ok", "fire": "ok", "smoke": "ok"}


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


def test_heartbeat_endpoint_accepts_report(client):
    r = client.post("/api/v1/device/heartbeat", json=_report().model_dump())
    assert r.status_code == 200
    assert client.get("/api/v1/devices/v831-1/health").json()["link"] == "online"
```

- [ ] **Step 2: 运行测试确认失败**

Run: `python -m pytest hub/tests/test_health.py -q`
Expected: FAIL，导入错误或健康路由 `404`

- [ ] **Step 3: 实现 `health.py` 与相关路由**

陈旧判断用报告自身的 `uptime_ms` 差值（`uptime_ms - *_last_detection_ms`），不依赖集线器时钟，避免设备与集线器时间不一致造成误判。链接状态用 `now - last_seen_at`。未上报的检测项为 `unknown`。路由按 `Settings` 传入阈值。

- [ ] **Step 4: 运行测试确认通过**

Run: `python -m pytest hub/tests/test_health.py -q`
Expected: PASS

- [ ] **Step 5: 提交**

```bash
git add hub/homevideo_hub/health.py hub/homevideo_hub/api.py hub/tests/test_health.py
git commit -m "feat(hub): device heartbeat and derived health states"
```

---

### Task 7: 布防配置与确认

**Files:**
- Create: `hub/homevideo_hub/arm.py`
- Modify: `hub/homevideo_hub/api.py`
- Test: `hub/tests/test_arm.py`

**Interfaces:**
- Consumes: `models.ArmRequest`、`models.ArmAck`、`models.ArmMode`
- Produces:
  - `arm.get_desired(conn) -> tuple[ArmMode, int]`（无记录时 `("unknown", 0)`）
  - `arm.set_desired(conn, mode: Literal["home","away"], now: float) -> tuple[ArmMode, int]`（版本号加一）
  - `arm.record_ack(conn, ack: ArmAck, now: float) -> bool`（`applied_version >= desired_version` 才生效）
  - `arm.arm_status(conn) -> dict`：`{"desired": {"mode","config_version"}, "applied": {...}|None, "effective": bool}`
  - 路由 `GET /api/v1/arm`、`PUT /api/v1/arm`、`POST /api/v1/arm/ack`、`GET /api/v1/arm/status`

- [ ] **Step 1: 写失败测试**

`hub/tests/test_arm.py`：

```python
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
```

- [ ] **Step 2: 运行测试确认失败**

Run: `python -m pytest hub/tests/test_arm.py -q`
Expected: FAIL，路由 `404`

- [ ] **Step 3: 实现 `arm.py` 与路由**

`arm_config` 表固定单行（`id=1`）。`PUT` 每次递增 `config_version`。`arm_ack` 表保存设备最近确认。`effective` 仅当确认版本不低于当前期望版本时为真。

- [ ] **Step 4: 运行测试确认通过**

Run: `python -m pytest hub/tests/test_arm.py -q`
Expected: PASS

- [ ] **Step 5: 提交**

```bash
git add hub/homevideo_hub/arm.py hub/homevideo_hub/api.py hub/tests/test_arm.py
git commit -m "feat(hub): arm config with device acknowledgement"
```

---

### Task 8: 通知接口

**Files:**
- Create: `hub/homevideo_hub/notify.py`
- Modify: `hub/homevideo_hub/api.py`
- Test: `hub/tests/test_notify.py`

**Interfaces:**
- Consumes: `Settings.notification_dir`、`Settings.notifier`
- Produces:
  - `notify.Notifier`：`Protocol`，方法 `notify(self, event: dict) -> None`
  - `notify.FileNotifier(directory: Path)`：把事件写为 `{event_id}.json`
  - `notify.get_notifier(settings: Settings) -> Notifier`
  - `api.create_app` 在事件**首次**创建时调用通知一次；重复上传不重复通知

- [ ] **Step 1: 写失败测试**

`hub/tests/test_notify.py`：

```python
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
```

- [ ] **Step 2: 运行测试确认失败**

Run: `python -m pytest hub/tests/test_notify.py -q`
Expected: FAIL，通知目录为空

- [ ] **Step 3: 实现 `notify.py` 与接入点**

`get_notifier` 按 `settings.notifier` 返回实现；未知名称返回一个抛出异常的 `Notifier`（用于测试通知失败不影响入库）。调用通知时捕获异常并记录日志，不改变 HTTP 响应。

- [ ] **Step 4: 运行测试确认通过**

Run: `python -m pytest hub/tests/test_notify.py -q`
Expected: PASS

- [ ] **Step 5: 提交**

```bash
git add hub/homevideo_hub/notify.py hub/homevideo_hub/api.py hub/tests/test_notify.py
git commit -m "feat(hub): pluggable notifier with file channel"
```

---

### Task 9: 保留期与容量清理

**Files:**
- Create: `hub/homevideo_hub/retention.py`
- Modify: `hub/homevideo_hub/api.py`
- Test: `hub/tests/test_retention.py`

**Interfaces:**
- Consumes: `Settings.retention_days`、`Settings.max_event_bytes`、`Settings.media_dir`、`events`/`snapshots`/`segments` 表
- Produces:
  - `retention.run_cleanup(conn, settings: Settings, now: float) -> dict`，返回 `{"deleted_events": int, "bytes_freed": int}`
  - `retention` 在删除时写 `deletions(event_id, reason, bytes_freed, created_at)` 并删除媒体文件
  - 路由 `POST /api/v1/maintenance/cleanup` 返回清理结果

- [ ] **Step 1: 写失败测试**

`hub/tests/test_retention.py`：

```python
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
    _age(conn, eid, 8 * DAY)
    result = run_cleanup(conn, settings, time.time())
    assert result["deleted_events"] == 1
    assert result["bytes_freed"] > 0
    assert client.get(f"/api/v1/events/{eid}").status_code == 404
    assert list(settings.segment_dir.glob(f"{eid}*")) == []


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
```

- [ ] **Step 2: 运行测试确认失败**

Run: `python -m pytest hub/tests/test_retention.py -q`
Expected: FAIL，`run_cleanup` 不存在

- [ ] **Step 3: 实现 `retention.py` 与维护路由**

删除条件：`status='closed'` 且 `received_at < now - retention_days*86400`。删除顺序为先删片段与截图文件，再删数据库行，再记 `deletions`。`bytes_freed` 为文件字节合计。清理永不删除 `status='active'` 的事件。连接从 `app.state.conn` 获取（Task 1 已在工厂中赋值）。

- [ ] **Step 4: 运行测试确认通过**

Run: `python -m pytest hub/tests/test_retention.py -q`
Expected: PASS

- [ ] **Step 5: 提交**

```bash
git add hub/homevideo_hub/retention.py hub/homevideo_hub/api.py hub/tests/test_retention.py
git commit -m "feat(hub): retention and capacity cleanup"
```

---

### Task 10: 端到端模拟设备流程

**Files:**
- Create: `hub/tests/test_end_to_end.py`
- Modify: `hub/README.md`（安装、运行与协议说明）

**Interfaces:**
- Consumes: 全部前置任务的 HTTP 接口
- Produces: 无新代码接口；本测试同时作为设备实现方的协议参考

- [ ] **Step 1: 写失败测试**

`hub/tests/test_end_to_end.py`：

```python
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
```

- [ ] **Step 2: 运行测试确认失败**

Run: `python -m pytest hub/tests/test_end_to_end.py -q`
Expected: FAIL，直到前面所有任务完成（此时应能定位到缺失接口）

- [ ] **Step 3: 补齐缺口并编写 `hub/README.md`**

README 写明：安装（`python -m venv .venv && pip install -e "hub[dev]"`）、运行（`uvicorn homevideo_hub.api:create_app --factory`）、数据目录与全部配置项及其环境变量、设备协议（端点、字段、幂等与校验规则、错误码 404/409/400/507）、健康状态含义，以及“确认后才可清理”的时序。

- [ ] **Step 4: 运行全部测试确认通过**

Run: `python -m pytest hub/tests -q`
Expected: PASS，全部用例通过

- [ ] **Step 5: 提交**

```bash
git add hub/tests/test_end_to_end.py hub/README.md
git commit -m "test(hub): end-to-end simulated device flow and protocol docs"
```

---

## 验证方式

每个任务用 `python -m pytest hub/tests -q` 运行，期望全部通过；任务内单文件命令见各步骤。完成全部任务后，`hub/` 目录可在无硬件环境下完整测试。

## 后续计划

- **子系统 A2（实时视频与远程访问）**：待 V831 能输出压缩流后，制定 go2rtc 接入、按需连接释放、Tailscale 组网、HTTPS 入口与延迟/首帧实测的任务。
- **子系统 B（V831 设备软件）**：待方案 §8 阶段 A 完成，取得固件、SDK、硬件编码接口与模型转换结论后制定。
- **子系统 C（手机网页界面）**：基于本计划的 API 制定。
- **子系统 D（板卡能力验证）**：需要实机，单独制定。
