# HomeVideo 设备端（V831）

运行在 Sipeed Maix-II Dock（V831）上的长驻设备程序：采集、编码、检测、事件录像，
并与树莓派集线器对接。它是方案中的**子系统 B**；树莓派集线器是**子系统 A**，协议见
`hub/README.md`。本批交付的是**不依赖硬件的骨架**——三个硬件后端以桩占位，待设计文档
§8.1 阶段 A 实测后补齐（见文末“待实机项”）。

设备 ID、协议字段与集线器逐字对齐：事件类型固定 `person_intrusion` /
`fire_suspected` / `smoke_suspected`，布防模式固定 `home` / `away` / `unknown`。

## 构建

开发机（推荐用仓库根的 `.venv`，它自带 cmake）：

```bash
cmake -S device -B build/device -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/device -j
ctest --test-dir build/device --output-on-failure
```

单元测试（`ctest`）覆盖配置、时钟、队列、分段与清单、事件状态机、落盘与配额、
集线器协议客户端、上传退避、健康与布防，全部在开发机上运行，不需要硬件，也不需要
集线器。协议客户端测试用 `httplib::Server` 起本地桩集线器。

交叉编译到 V831（仅有工具链文件，无第三方依赖；`musl` 环境无包管理器，所以
`cpp-httplib` 与 `nlohmann/json` 都已 vendored 到 `device/third_party/`）：

```bash
cmake -S device -B build/v831 -DCMAKE_TOOLCHAIN_FILE=device/cmake/toolchain-v831.cmake
```

**此交叉构建在阶段 A 通过前无法在真机上运行。** 它只证明“能编过”，不证明“能采到图、
能编出 H.264、能跑起模型”——那三件事的结论见 §8.1（待实机项）。

## 运行

```
hv_device [--config PATH] [--fake-device] [--once] [--once-budget-ms N] [--verbose] [--help]
```

- `--config PATH`：JSON 配置文件；缺省时只用内置默认值与环境变量。
- `--fake-device`：使用合成摄像头、桩编码器与可编排的桩检测器，无需硬件。
- `--once`：跑一个短回合后退出 0，供联调脚本使用；`--once-budget-ms N` 是该回合的
  墙钟上限。
- `--verbose`：提高日志级别。

真实设备上，配置文件是 `device/config/example.json` 的拷贝（放在 SD 卡上，例如
`/data/homevideo/device.json`）。

```bash
build/device/hv_device --config device/config/example.json          # 真机
build/device/hv_device --config device/config/example.json \
  --fake-device --once --once-budget-ms 12000                       # 开发机联调
```

### 配置与优先级

配置来自三层，后者覆盖前者：**内置默认值 < 配置文件 < `HOMEVIDEO_*` 环境变量**。
环境变量只覆盖下面列出的字段；其余字段（如 `encode.codec`、`detect.detectors`）只能
来自文件或默认值。布尔值接受 `1/0`、`true/false`、`yes/no`、`on/off`。

| 环境变量 | 字段 |
| --- | --- |
| `HOMEVIDEO_DEVICE_ID` | `device_id` |
| `HOMEVIDEO_HUB_URL` | `hub_url` |
| `HOMEVIDEO_SPOOL_DIR` | `spool_dir` |
| `HOMEVIDEO_LOG_LEVEL` | `log_level`（`debug`\|`info`\|`warn`\|`error`） |
| `HOMEVIDEO_CAPTURE_WIDTH` / `HOMEVIDEO_CAPTURE_HEIGHT` / `HOMEVIDEO_CAPTURE_FPS` | `capture.*` |
| `HOMEVIDEO_CAPTURE_REALTIME` | `capture.realtime` |
| `HOMEVIDEO_ENCODE_BITRATE_BPS` / `HOMEVIDEO_ENCODE_GOP_MS` | `encode.*` |
| `HOMEVIDEO_DETECT_INTERVAL_MS` / `HOMEVIDEO_DETECT_CONFIRM_FRAMES` / `HOMEVIDEO_DETECT_CONFIRM_HOLD_MS` | `detect.*` |
| `HOMEVIDEO_EVENT_PRE_ROLL_MS` / `_POST_ROLL_MS` / `_SEGMENT_MS` / `_SEGMENT_BYTES` / `_MERGE_WINDOW_MS` / `_RING_BUFFER_BYTES` | `event.*` |
| `HOMEVIDEO_UPLOAD_HEARTBEAT_MS` / `_ARM_POLL_MS` / `_MAX_BACKOFF_MS` / `_MAX_ATTEMPTS` / `_RATE_BYTES_PER_S` / `_HTTP_TIMEOUT_MS` | `upload.*` |
| `HOMEVIDEO_QUOTA_ROLLING_BYTES` / `_PENDING_BYTES` / `_UPLOADED_BYTES` / `_METADATA_BYTES` | `quota.*` |
| `HOMEVIDEO_HEALTH_VIDEO_STALE_MS` / `_RECORDING_STALE_MS` / `_MAX_STAGE_RESTARTS` | `health.*` |

非法值（`fps` 为 0、`hub_url` 没有 `http(s)://`、`merge_window_ms < post_roll_ms` 等）
会让程序以非零退出并打印出错字段。`merge_window_ms >= post_roll_ms` 是硬约束：事件
不能在补录尾巴录完之前关闭，否则间歇性触发会被拆成多个事件。

`device/config/example.json` 是完整字段的板端示例（`v831a`、集线器 URL、
`/data/homevideo/spool`、`/data/homevideo/models` 下的三个模型）。JSON 没有注释，每个
字段都自解释，字段含义见上表与 `device/include/hv/config.h`。

## 落盘布局

```
<spool_dir>/
  events/{event_id}/manifest.json    # 事件元数据 + 编码参数 + 片段清单与上传状态
  events/{event_id}/snapshot.jpg     # 事件截图（Content-Type 决定扩展名）
  events/{event_id}/seg_0000.bin     # 事件片段，从关键帧开始
  state/device_state.json            # boot_id、已确认布防模式与版本、时间基准
  lost.json                          # 配额淘汰的丢失计数（事件/字节）
```

`manifest.json` 用“先写临时文件再 rename”的原子方式更新，包含：`manifest_version`；
`event`（`event_id`、`device_id`、`event_type`、`started_at`、`time_trusted`、
`device_boot_id`、`device_started_ms`、`model_version`、`config_version`、`arm_mode`、
`detections[]`）；`codec`（`format`/`width`/`height`/`fps`/`bitrate_bps`/`gop_ms`）；
`segments[]`（`seq`、`file`、`bytes`、`sha256`、`keyframe`、`start_ms`、`end_ms`、
`uploaded`）；`state`；以及关闭后的 `closed`（`ended_at`、`duration_ms`、
`time_trusted`）。片段自带关键帧起始与编码参数，集线器将来可仅重封装而不解码。

**只有集线器确认持久保存后，设备才清理本地副本**；未确认的内容一律保留以便重传。配额
分滚动缓冲、待上传、已上传、元数据四个桶，淘汰顺序为：滚动缓冲 → 已上传事件 → 待上传
的普通事件 → **待上传的疑似火灾事件最后**；正在写入、正在上传或被未完成事件引用的文件
不清理。

## 端到端联调

`device/tools/e2e_with_hub.sh` 针对**真实集线器**验证设备，可重复且自清理：

1. 从脚本自身位置解析仓库根；要求仓库根有 `.venv`（内含 `uvicorn`），否则明确报错。
2. 在一个临时目录启动真实集线器
   （`HOMEVIDEO_DATA_DIR=<tmp> uvicorn homevideo_hub.api:create_app --factory`），
   用有界轮询等 `/api/v1/health` 就绪（不是裸 `sleep`）。
3. 写入一份临时设备配置（临时 `spool_dir`、`capture.realtime=false`、很小的
   `pre/post_roll_ms` 与 `merge_window_ms`、`confirm_frames=1`、`confirm_hold_ms=0`、
   短 `interval_ms`，并脚本化一次 `fire_suspected` 检测——火灾不受布防门控，保证必然
   产生事件），把集线器 `PUT /api/v1/arm` 为 `away`。
4. 构建缺失的 `build/device/hv_device`，然后跑
   `hv_device --config <tmp>/device.json --fake-device --once --once-budget-ms 12000`。
5. 用 `curl` + `python3` 断言（无 `jq` 依赖）：
   - 集线器收到至少一个事件，详情里 `snapshot` 非空、`segments` 至少一段；
   - 事件 `status=="closed"` 且有 `duration_ms`；
   - 再跑一次设备不会给**同一事件**产生第二行，且重复上传同一元数据/片段是可幂等的
     空操作（同事件换 boot id 产生的**新**事件是正常行为，脚本据此说明而非判失败）；
   - `GET /api/v1/devices/{id}/health` 的 `link=="online"`；
   - `PUT /arm=away` 后设备已确认，`/arm/status` 报 `effective==true`。

每条断言打印 PASS/FAIL，末尾给出汇总；任一失败退出非零，并附带集线器日志尾部。脚本用
`trap` 保证无论成功失败都杀掉集线器并删除临时目录。

脚本也可以挂到 `ctest` 上，但默认关闭——单元测试必须只靠编译器就能跑：

```
cmake -S device -B build/device -DHV_E2E=ON
ctest --test-dir build/device -R device_hub_e2e --output-on-failure
```

## 待实机项

- `device/src/platform/v831_camera.cpp`、`v831_encoder.cpp`、`v831_detector.cpp` 目前是
  桩，调用即返回 `Code::Internal`。桌面构建不实例化它们。
- **编码路径取决于 `docs/homevideo-device-design.md` §8.1 的实测结论。** 该节先枚举镜像
  实际提供的编码能力（`opkg list | grep -i mpp`、`ls /dev | grep -iE 'cedar|venc|mpp'`、
  `which ffmpeg` 等），再决定编码实现路径：有 MPP/venc 库就直接用 C 接口，只有运行库
  没有头文件就从 Tina SDK 取头文件交叉编译。硬件编码是否可用必须实测确认，不能依据
  芯片标称能力推断。
- 摄像头能否编码与检测共享一路采集、实际关键帧间隔与可控性、NPU 运行时与模型转换、
  三个模型同时驻留的内存开销、64MB 内存下的并行余量，同样来自阶段 A 实测，直接决定
  `platform/v831_*` 与队列/缓冲的最终形态。硬件后端是下一步工作。

## 已知缺口

- **集线器目前没有任何认证**（见 `hub/README.md` 已知缺口）。设备以普通 HTTP 访问，
  因此集线器不得暴露在 localhost 或 Tailscale 之外；认证落地前只应指向局域网或
  Tailscale 地址。
- 管线里当前存在一次整帧拷贝，是骨架阶段的临时决定；等 MPP 数据路径确定后（阶段 A）
  再决定如何消除。
- 实时视频转发（子系统 A2）与手机网页（子系统 C）尚未实现；设备端不做 MP4 封装、转码
  或解码，也不长期保存普通视频（滚动缓冲只保留事件前窗口）。
