# HomeVideo 集线器（树莓派）

轻量的家庭视频事件集线器：接收 V831 设备的心跳、事件元数据、截图与视频片段，
幂等落盘，并对外提供事件回溯、布防配置、设备健康与容量清理接口。

- 单一 FastAPI 应用（应用工厂 + 依赖注入），HTTP 层与业务逻辑分离。
- 元数据存 SQLite（标准库 `sqlite3`），媒体文件存文件系统。
- 所有写入**先落盘并提交数据库，再向设备确认**；确认后设备才可清理本地副本。
- 不引入数据库服务、消息队列或转码组件；集线器不常驻解码、转码或推理。

## 安装

需要 Python 3.11+。在仓库根目录执行：

```bash
python -m venv .venv
source .venv/bin/activate
pip install -e "hub[dev]"
```

`hub[dev]` 会安装运行依赖（`fastapi`、`uvicorn[standard]`、`pydantic>=2`）与开发依赖
（`pytest`、`httpx`，用于 `TestClient`）。

## 运行

在仓库根目录、虚拟环境已激活的前提下：

```bash
uvicorn homevideo_hub.api:create_app --factory
```

`--factory` 表示 `create_app` 是返回 FastAPI 实例的工厂函数。默认监听
`127.0.0.1:8000`；如 `uvicorn --host 0.0.0.0 --port 8000
homevideo_hub.api:create_app --factory`。生产部署应由 systemd 等进程管理器
托管，并在前面加 HTTPS 入口（实时视频转发另行规划，不在本服务内）。

## 测试

在仓库根目录执行：

```bash
python -m pytest hub/tests -q
```

全部测试使用模拟设备驱动真实 HTTP 接口，无需硬件、无需数据库服务。
`hub/tests/test_end_to_end.py` 覆盖一次完整的设备流程，同时可作为设备实现方的协议参考。

## 数据目录与配置

默认数据目录为 `data/`（相对当前工作目录）。目录结构与生成的文件：

```
data/
  hub.db                      # SQLite 元数据库（WAL 模式）
  media/
    snapshots/{event_id}{ext} # 截图，扩展名由 Content-Type 决定
    segments/{event_id}_{seq}.bin  # 视频片段
  notifications/{event_id}.json    # 文件通知（默认通知通道）
```

全部配置项均可通过 `HOMEVIDEO_*` 环境变量覆盖：

| 环境变量 | 默认值 | 含义 |
| --- | --- | --- |
| `HOMEVIDEO_DATA_DIR` | `data` | 数据目录；显式传入的 `data_dir` 参数优先于它 |
| `HOMEVIDEO_RETENTION_DAYS` | `7` | 保留天数；仅早于该窗口的**已关闭**事件会被清理 |
| `HOMEVIDEO_MAX_EVENT_BYTES` | `2000000000` | 单个事件截图 + 片段累计字节上限，超出拒收 |
| `HOMEVIDEO_METADATA_RESERVE_BYTES` | `67108864`（64 MiB） | 为元数据预留、不参与媒体写入的磁盘余量 |
| `HOMEVIDEO_HEARTBEAT_TIMEOUT_SECONDS` | `30` | 心跳静默超过该秒数判定设备 `offline` |
| `HOMEVIDEO_DETECTION_STALE_SECONDS` | `5` | 检测器距上次完成的时长超过该秒数判定 `stale` |
| `HOMEVIDEO_NOTIFIER` | `file` | 通知通道；当前仅实现 `file` |

截图扩展名按 `Content-Type` 映射：`image/jpeg` → `.jpg`，`image/png` → `.png`，
其余 → `.bin`。片段文件名固定为 `{event_id}_{seq}.bin`。

## 并发模型

FastAPI 在线程池中执行同步路由。**每个请求由依赖注入打开自己的 SQLite 连接**
（经 `homevideo_hub.db.connect`，带 WAL 日志、`row_factory` 与
`PRAGMA foreign_keys=ON`），请求结束后关闭，因此并发请求不会共享同一个连接的
隐式事务状态（一个线程的 `with conn:` 不会提交或回滚另一个线程的事务）。工厂
启动时打开的 `app.state.conn` 仍然保留，仅供测试与运维工具直接读写使用。

## 设备协议

所有路径前缀 `/api/v1`。请求体为 JSON；截图与片段以原始字节流上传。

### 端点

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| POST | `/device/heartbeat` | 上报设备心跳与健康进度，200 返回 `{device_id, status}` |
| GET | `/devices/{device_id}/health` | 单个设备的派生健康状态 |
| GET | `/health` | 所有已知设备的健康状态 `{devices: [...]}` |
| GET | `/arm` | 读取期望布防配置 `{mode, config_version}` |
| PUT | `/arm` | 设置期望布防配置，请求 `{mode: "home"\|"away"}`；每次调用自增版本 |
| POST | `/arm/ack` | 设备确认已应用配置 `{device_id, applied_mode, applied_version}`；返回 `{status, effective}` |
| GET | `/arm/status` | 期望与已确认配置对照 `{desired, applied, effective, acked_devices}` |
| POST | `/events` | 事件元数据入库（幂等），返回 `{event_id, created}` |
| POST | `/events/{event_id}/snapshot` | 上传截图，`Content-Type` 决定扩展名 |
| PUT | `/events/{event_id}/segments/{seq}` | 上传片段，请求头 `X-Segment-Sha256` 为声明的十六进制校验值 |
| POST | `/events/{event_id}/close` | 关闭事件，请求 `{ended_at, duration_ms, time_trusted}` |
| GET | `/events` | 事件列表，支持 `event_type`、`status`、`limit`（默认 50，范围 1–500）、`offset`（≥ 0） |
| GET | `/events/{event_id}` | 事件详情，含 `snapshot`（或 `null`）与按 `seq` 升序的 `segments` 列表 |
| POST | `/maintenance/cleanup` | 执行保留期清理，返回 `{deleted_events, bytes_freed}` |

### 事件元数据字段（POST /events）

`event_id`、`device_id`、`event_type`（三值之一）、`started_at`（可空）、
`time_trusted`（必填）、`device_boot_id`、`device_started_ms`、`model_version`、
`config_version`、`arm_mode`、`detections`。每个 detection 为
`{label, confidence, bbox?, zone?}`。

事件类型固定为三值：`person_intrusion`、`fire_suspected`、`smoke_suspected`。
布防模式固定为三值：`home`、`away`、`unknown`。

### 幂等与校验规则

- **事件**：同一 `event_id` 重复 POST 只刷新元数据，不新增第二行。首次插入返回
  201（`created: true`），重复返回 200（`created: false`）。
- **截图**：每个事件至多一张。首次返回 201（`status: "stored"`）；该事件已有截图时
  直接返回 200（`status: "duplicate"`），不写第二个文件或行。截图上传不携带声明
  校验值，因此不做校验值比对——一个事件只保留一张截图。
- **片段**：以 `(event_id, seq)` 唯一。相同序号、相同校验值重传为幂等重复，返回 200
  （`status: "duplicate"`）；相同序号、不同校验值为冲突，返回 409 且不覆盖。序号可
  乱序到达。
- **关闭**：可重复调用，幂等；无片段也可关闭。
- **时钟不可信**：`time_trusted=false` 时，`started_at`/`ended_at` 一律存为 `NULL`，
  但 `duration_ms` 始终记录；列表按集线器收到的 `received_at` 排序，而非设备时钟。
- **流式与原子性**：上传边收边写，绝不把完整录像读入内存；先写临时文件，再用原子
  重命名发布到最终路径。校验失败、容量不足等上传阶段的失败路径都会删除临时文件，
  不会留下半截文件。需要如实说明的边界：截图与片段都是“先原子重命名、后提交数据库
  行”，若在重命名之后、INSERT 提交之前进程失败，可能残留一个已改名但没有对应数据库
  行的孤儿文件；保留期清理只删除数据库行引用的路径，不会回收这类孤儿文件（需人工或
  后续的对账任务处理）。
- **容量**：截图与片段上传都会先检查磁盘余量（扣除元数据预留），再检查单事件累计
  字节上限（截图 + 全部片段）；任一不足即拒收并返回 507，且不落盘、不写行。

### 错误码

| 状态码 | 含义 |
| --- | --- |
| 400 | 声明校验值与实际不符（`checksum mismatch`） |
| 404 | 事件不存在（尚未收到元数据）或详情查询的 `event_id` 未知 |
| 409 | 同序号片段已存在且校验值不同（`segment conflict`） |
| 422 | 请求体或查询参数未通过校验（如 `event_id` 含非法字符、`limit` 越界） |
| 507 | 磁盘余量（扣除预留后）不足，或本次上传会使单事件累计字节超过上限 |

### “确认后才可清理”的时序

1. 设备上传事件元数据、截图或片段。
2. 集线器把文件写入最终路径（原子替换）并提交数据库行。
3. **只有在持久保存之后**，集线器才返回 201/200 确认。
4. 设备收到确认后，才可以清理本地副本；未获确认的内容必须保留以便重传。
5. 集线器的保留期清理只删除 `status='closed'` 且早于保留窗口的事件，`active`
   事件永不被清理。

## 布防与检测语义

**布防**：集线器区分“期望（desired）”与“已确认（applied）”。`PUT /arm` 只更新期望
配置并自增 `config_version`；只有设备用 `POST /arm/ack` 确认与当前期望模式一致、且
`applied_version >= desired` 时，`/arm/status` 的 `effective` 才为 `true`。未初始化
（`unknown`、版本 0）**绝不显示为已布防或已生效**；过期或落后的确认也不生效。断网时
设备继续执行最后一次确认的配置。

**健康**：`GET /devices/{id}/health` 返回四个彼此独立的维度，各有自己的取值集合：

- `link`：`online` / `offline` / `unknown`。依据集线器时钟上距上次心跳的时长，超过
  `HOMEVIDEO_HEARTBEAT_TIMEOUT_SECONDS` 为 `offline`；从未上报为 `unknown`（绝不当作在线）。
- `video`：`ok` / `failed` / `unknown`，来自心跳中的 `video_ok` 硬件标志。
- `recording`：`ok` / `failed` / `unknown`，来自 `recording_ok` 硬件标志。
- `detection`：`person`、`fire`、`smoke` 各为 `ok` / `stale` / `unknown`。判定依据是
  设备自身毫秒运行时钟的差值（`uptime_ms - last_detection_ms`），与集线器时钟无关；
  差值超过 `HOMEVIDEO_DETECTION_STALE_SECONDS` 为 `stale`。检测器没有独立的失败信号，
  因此“检测失效”表现为 `stale`，永远不会是 `failed`。

火灾检测始终开启，不受布防模式影响。

## 已知缺口 / 后续工作

本服务当前实现**没有任何身份认证或设备身份校验**：任何能访问该端口的客户端都可以
上报事件、上传媒体、修改布防配置或触发清理。因此集线器**不得暴露在 localhost 或
Tailscale 之外**，在认证落地之前不要绑定公网地址。认证是设计的要求（见设计文档
§6.2 / §7.1），并已在后续的远程访问（A2）工作中跟踪。
