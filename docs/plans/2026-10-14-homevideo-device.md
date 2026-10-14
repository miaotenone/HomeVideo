# HomeVideo 设备端（V831）软件 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 V831 上实现长驻设备程序：采集、编码、检测、事件录像与上传，与树莓派集线器（子系统 A）对接。本批交付**不依赖硬件的骨架**——接口、纯逻辑、桌面后端与设备↔集线器联调；硬件后端留桩待 §8 阶段 A 实测后补齐。

**Architecture:** C++17 + CMake。三个硬件接口（摄像头、编码器、检测器）是唯一接触硬件的边界，其余模块不感知硬件。每级一个线程 + 有界队列。事件状态机、落盘、配额、上传与健康全部为纯逻辑，可在开发机完整测试。HTTP/JSON 用两个 vendored header-only 库。

**Tech Stack:** C++17、CMake ≥ 3.16、C++ 标准线程与文件系统、vendored `cpp-httplib` + `nlohmann/json`、自带单头测试框架 + CTest；交叉工具链 `arm-openwrt-linux-muslgnueabi-`。

**Spec:** `docs/homevideo-device-design.md`（细节）与 `docs/homevideo-design.md`（方案，§4/§5/§7/§8 为本文约束来源）

## Global Constraints

以下取自方案与设计文档，逐字复制；每个任务都隐含包含本节。

- 事件类型固定为三值：`person_intrusion`、`fire_suspected`、`smoke_suspected`。
- 布防模式固定为三值：`home`、`away`、`unknown`；`unknown` 表示未初始化，不得上报为已布防或已生效。
- `event_id` 必须匹配 `^[A-Za-z0-9._-]{1,128}$`（集线器协议层校验，违者 422）。
- 事件前缓冲 10 秒、结束后补录 20 秒；长事件按约 60 秒分段，归属同一事件。
- 检测输入分辨率与编码分辨率分离；编码后的数据供录像与实时服务复用，不重复编码。
- 有界队列：模型推理、文件上传或慢速观看客户端不得无限堆积数据、阻塞摄像头。
- 录像从可解码的关键帧开始；片段必须保留最终封装所需的编码配置与时间信息。
- 正在写入、正在上传或被未完成事件引用的片段不能被清理；元数据使用原子更新。
- 必须先在集线器持久保存并确认后，设备才可清理本地副本；未获确认的内容必须保留以便重传。
- 上传重试采用**有上限的退避**，网络故障时不得持续占用 CPU。
- 火灾检测始终开启，不受布防模式影响；视觉结果表述为“疑似火灾”。
- 时间不可信时用启动标识、单调时钟及事件时长保存记录，不伪装为准确日期时间；系统时间校正不得改变用于判断持续时间的单调时钟。
- 设备端不常驻解码、不转码、不做 MP4 封装；不长期保存普通视频（滚动缓冲只保留事件前窗口）。
- 依赖上限：C++17；不引入数据库服务、消息队列、转码组件、第三方测试框架。

## 范围说明

方案覆盖多个子系统，按验证独立性拆分：

- **子系统 A：树莓派集线器（已完成）。** 提供本计划依赖的协议契约，见 `hub/README.md`。
- **子系统 B：V831 设备软件（本计划）。** 硬件相关后端待 §8 阶段 A 实测；本批交付骨架与联调。
- **子系统 A2：实时视频与远程访问。** 待设备能输出压缩流后制定。
- **子系统 C：手机网页。** 在 A 的 API 稳定后制定。
- **子系统 D：板卡能力验证脚本与记录表。** 依赖实机，单独制定。

## Review Focus

方案隐含、但容易做错、因而每个任务都显式测试的五类输入：

1. **时钟不可信与时间跳变**：`time_trusted=false` 时不得写 `started_at`/`ended_at`，但 `duration_ms` 必须正确；单调时钟不受墙钟校正影响。
2. **重复与乱序上传**：同 `(event_id, seq)` 同校验值＝成功、不同校验值＝409；片段先于元数据到达时先补元数据。
3. **磁盘与配额耗尽**：配额淘汰顺序正确（疑似火灾最后清理），正在上传的片段不被清理，丢失计数必须记录。
4. **未初始化状态**：从未确认布防时不得上报 `home`/`away`；从未触发的检测器 `last_detection_ms` 必须为 `null` 而不是 0。
5. **进程中断与重启**：未正常结束的事件重启后标记 `interrupted` 且已完成片段保留；布防状态持久恢复。

---

### Task 1: 工程骨架与构建系统

**Files:**
- Create: `device/CMakeLists.txt`
- Create: `device/cmake/toolchain-v831.cmake`
- Create: `device/third_party/httplib.h`、`device/third_party/json.hpp`（vendored，附 LICENSE 说明）
- Create: `device/.gitignore`
- Create: `device/include/hv/status.h`、`device/include/hv/log.h`
- Create: `device/src/log.cpp`
- Create: `device/tests/harness.h`
- Create: `device/tests/test_harness.cpp`

**Interfaces:**
- Consumes: 无
- Produces:
  - `hv::Status`（`enum class Code { Ok, Timeout, Io, Protocol, Conflict, NotFound, InsufficientStorage, Invalid, Internal }`）与 `hv::Result`（`Code` + `std::string msg` + `ok()`）
  - `hv::log`：`setup(LogLevel, bool to_stderr)`、`debug/info/warn/error(fmt, ...)`
  - 测试框架宏：`HV_TEST(name)`、`HV_EXPECT_TRUE/False/Eq/Ne/AlmostEq/StrEq`、`hv::test::run_all()`
  - CMake 目标 `hv_core`（静态库）、`hv_device`（可执行）、`hv_tests`（CTest）

**测试用例（`test_harness.cpp`）：**
- 断言通过时不报错；断言失败时记录失败并让进程返回非零。
- 断言失败不终止其余用例（跑完全部再汇总）。
- `Result` 的 `ok()` 与 `Code::Ok` 一致。

**Steps:**
- [ ] Step 1: 写 `tests/harness.h` 与 `tests/test_harness.cpp`（自测框架本身）
- [ ] Step 2: 运行确认失败 —— `cmake -S device -B build/device` 应因缺少 `CMakeLists.txt` 失败
- [ ] Step 3: 实现 `CMakeLists.txt`（`-Wall -Wextra`，`CXX_STANDARD 17`，`enable_testing()`，按平台选择后端源文件目录）、`toolchain-v831.cmake`、`status.h`、`log.h/cpp`；用 `curl` 下载并 vendored 两个头文件
- [ ] Step 4: `cmake --build build/device -j && ctest --test-dir build/device --output-on-failure` 全绿
- [ ] Step 5: 提交 `feat(device): scaffold CMake project, logging and test harness`

---

### Task 2: 设备配置

**Files:**
- Create: `device/include/hv/config.h`、`device/src/config.cpp`
- Test: `device/tests/test_config.cpp`

**Interfaces:**
- Consumes: `hv::Result`、`nlohmann/json`
- Produces:
  - `struct hv::DeviceConfig`：`device_id`、`hub_url`、`spool_dir`、`data_root`，以及分组子结构
    `capture`（`width`、`height`、`fps`）、`encode`（`bitrate_bps`、`gop_ms`、`codec`）、
    `detect`（`labels` 每标签的模型路径/输入尺寸/阈值、`interval_ms`、`confirm_frames`、`confirm_hold_ms`）、
    `event`（`pre_roll_ms`、`post_roll_ms`、`segment_ms`、`merge_window_ms`）、
    `upload`（`max_backoff_ms`、`max_attempts`、`rate_bytes_per_s`、`heartbeat_ms`、`arm_poll_ms`）、
    `quota`（四桶字节上限）、`health`（`video_stale_ms`、`recording_stale_ms`）
  - `hv::Result hv::load_config(const std::filesystem::path& file, const std::map<std::string,std::string>& env)`
  - `hv::DeviceConfig hv::default_config()`

**测试用例：**
- 默认值：`pre_roll_ms==10000`、`post_roll_ms==20000`、`segment_ms==60000`、`heartbeat_ms==10000`、`arm_poll_ms==5000`、编码 `1280x720/15fps/1500000bps`。
- 文件覆盖与 `HOMEVIDEO_*` 环境变量覆盖的优先级（env > 文件 > 默认）。
- 非法值（`fps==0`、`segment_ms==0`、`pre_roll_ms` 为负、`hub_url` 为空）返回 `Code::Invalid` 且信息含字段名。
- 未知字段被忽略而不报错；缺失的 `config_version` 允许。

**Steps:**
- [ ] Step 1: 写 `test_config.cpp` 覆盖上表用例
- [ ] Step 2: 运行确认失败
- [ ] Step 3: 实现 `config.h/cpp`
- [ ] Step 4: 运行 `ctest` 全绿
- [ ] Step 5: 提交 `feat(device): device configuration with file and env overrides`

---

### Task 3: 时钟与时间可信

**Files:**
- Create: `device/include/hv/clock.h`、`device/src/clock.cpp`
- Test: `device/tests/test_clock.cpp`

**Interfaces:**
- Consumes: 无
- Produces:
  - `class hv::IClock`：`virtual MonotonicMs monotonic_ms() const`、`virtual int64_t unix_seconds() const`
  - `class hv::SystemClock : IClock`：单调用 `steady_clock`，墙钟用 `system_clock`
  - `class hv::FakeClock : IClock`：测试用，可手动推进并制造跳变
  - `class hv::TimeSource`：`void observe_server_unix(int64_t)`、`bool trusted() const`、
    `std::optional<double> started_at_unix() const`（不可信返回 `nullopt`）、`MonotonicMs uptime_ms() const`
  - `std::string hv::new_boot_id()`（8 位十六进制）、`std::string hv::make_event_id(device_id, boot_id, MonotonicMs, uint32_t seq)`（匹配 `^[A-Za-z0-9._-]{1,128}$`）

**测试用例：**
- `SystemClock::monotonic_ms()` 单调不减；`FakeClock` 推进后墙钟跳变不影响单调值。
- 未观测到服务器时间时 `trusted()==false`、`started_at_unix()==nullopt`。
- 观察到 `Date` 时间且与本地偏差 < 阈值时 `trusted()==true`，`started_at_unix()` 有值。
- 偏差超过阈值（如服务器时间落后 1 小时）时仍 `trusted()==false`。
- 制造墙钟跳变（`FakeClock` 墙钟 +3600s）后 `uptime_ms()` 不受影响。
- `make_event_id` 长度 ≤128、只含允许字符，且同参数不同 `seq` 生成不同 ID（正则断言）。

**Steps:**
- [ ] Step 1: 写 `test_clock.cpp`
- [ ] Step 2: 运行确认失败
- [ ] Step 3: 实现 `clock.h/cpp`
- [ ] Step 4: `ctest` 全绿
- [ ] Step 5: 提交 `feat(device): monotonic clock, time-trust tracking and event ids`

---

### Task 4: 有界队列

**Files:**
- Create: `device/include/hv/queue.h`
- Test: `device/tests/test_queue.cpp`

**Interfaces:**
- Consumes: 无
- Produces:
  - `enum class hv::OverflowPolicy { DropOldest, DropNewest, Reject }`
  - `template <class T> class hv::BoundedQueue`：`BoundedQueue(size_t capacity, OverflowPolicy)`、
    `PushOutcome push(T)`（`Accepted | DroppedOld | DroppedNew | Rejected`）、
    `bool pop(T& out, int timeout_ms)`、`void close()`、`bool closed() const`、
    `struct Stats { uint64_t pushed, popped, dropped, rejected; size_t depth; }`、`Stats stats() const`

**测试用例：**
- 容量 2 的 `Reject` 队列：第三次 push 返回 `Rejected`、`dropped==0`、深度仍为 2。
- `DropOldest`：丢弃最旧元素，后续 `pop` 得到最新的两个。
- `DropNewest`：保留最旧，返回 `DroppedNew`。
- `pop` 超时返回 false 且不改变统计；`close()` 后 `pop` 立即返回 false（不阻塞到超时）。
- 多生产者/单消费者下总入队 == 出队 + 丢弃 + 拒绝 + 深度（不丢账）。
- `stats()` 在并发推进时至少单调不降。

**Steps:**
- [ ] Step 1: 写 `test_queue.cpp`
- [ ] Step 2: 运行确认失败
- [ ] Step 3: 实现 `queue.h`（`std::mutex` + `condition_variable`）
- [ ] Step 4: `ctest` 全绿
- [ ] Step 5: 提交 `feat(device): bounded queue with drop policies and accounting`

---

### Task 5: 硬件接口与桌面后端

**Files:**
- Create: `device/include/hv/types.h`、`device/include/hv/platform.h`
- Create: `device/src/platform/desktop_camera.cpp`、`desktop_encoder.cpp`、`desktop_detector.cpp`
- Create: `device/src/platform/v831_camera.cpp`、`v831_encoder.cpp`、`v831_detector.cpp`（返回 `Code::Internal`，注释指向阶段 A）
- Create: `device/src/platform/factory.cpp`（按构建开关选择后端）
- Test: `device/tests/test_platform.cpp`

**Interfaces:**
- Consumes: `hv::Result`、`hv::IClock`
- Produces:
  - `types.h`：`using MonotonicMs = int64_t`、`enum class PixelFormat { Yuv420, Rgb888 }`、
    `enum class EventType { PersonIntrusion, FireSuspected, SmokeSuspected }`、
    `enum class ArmMode { Home, Away, Unknown }`、`to_wire/from_wire` 转换、
    `struct Frame`、`struct EncodedChunk { MonotonicMs ts_ms; bool keyframe; std::vector<uint8_t> data; }`、
    `struct Detection { EventType label; float confidence; std::optional<BBox> bbox; std::optional<std::string> zone; MonotonicMs ts_ms; }`、
    `struct EncoderConfig`、`struct DetectorConfig`
  - `platform.h`：`ICameraSource`、`IEncoder`、`IDetector`（见设计文档 §4）
  - `create_camera/create_encoder/create_detector(const DetectorConfig&)` 工厂
- 桌面后端语义：合成摄像头按 `fps` 产出带递增时间戳的合成帧；桩编码器按 `gop_ms` 产出关键帧标记与可复现的伪码流（内容由帧序号决定，便于断言 sha256）；桩检测器可脚本化为“第 N 次运行返回给定结果”。

**测试用例：**
- `to_wire(EventType::FireSuspected)=="fire_suspected"`，`from_wire("smoke_suspected")` 正确；三值互转往返一致。
- `to_wire(ArmMode::Unknown)=="unknown"`；非法字符串返回 `nullopt`。
- 合成摄像头连续 `read` 的时间戳严格递增且间隔≈`1000/fps`。
- 桩编码器：`gop_ms=2000`、`fps=15` 时每 30 帧恰好一个关键帧，第一帧为关键帧。
- 桩编码器 `codec_config()` 非空且在 `open` 后即可用。
- 桩检测器只产出所属标签的 `Detection`。
- v831 后端在桌面构建下不可用（工厂不返回它们）；直接实例化返回 `Code::Internal`。

**Steps:**
- [ ] Step 1: 写 `test_platform.cpp`
- [ ] Step 2: 运行确认失败
- [ ] Step 3: 实现 `types.h`、`platform.h`、桌面后端与 v831 桩
- [ ] Step 4: `ctest` 全绿
- [ ] Step 5: 提交 `feat(device): platform interfaces with desktop backends and v831 stubs`

---

### Task 6: 环形缓冲、分段器与清单

**Files:**
- Create: `device/include/hv/recorder.h`、`device/src/recorder.cpp`
- Test: `device/tests/test_recorder.cpp`

**Interfaces:**
- Consumes: `EncodedChunk`、`IClock`、`json.hpp`
- Produces:
  - `class hv::RingBuffer`：`explicit RingBuffer(size_t max_bytes)`、`void push(EncodedChunk)`、
    `std::vector<const EncodedChunk*> since(MonotonicMs ts) const`、`size_t bytes() const`、
    `void clear()`、`uint64_t evicted_chunks() const`（按字节上限淘汰最旧，至少保留一个关键帧起点）
  - `struct hv::SegmentInfo { int seq; std::string file; uint64_t bytes; std::string sha256; bool keyframe; MonotonicMs start_ms, end_ms; bool uploaded; }`
  - `class hv::Segmenter`：`Segmenter(MonotonicMs segment_ms, std::vector<uint8_t> codec_config)`、
    `std::optional<SegmentInfo> add(const EncodedChunk&)`（收满或遇关键帧且已满 `segment_ms` 时吐出；
    关键帧起始段前置 `codec_config`）、`std::optional<SegmentInfo> flush()`、
    `void begin_segment_at_keyframe()`（事件起点对齐关键帧）
  - `hv::Manifest`：`to_json` / `from_json`，字段与设计文档 §7 一致；`bool save_atomic(path) const`
    （先写 `.tmp` 再 `fs::rename`）
  - `std::string hv::sha256_hex(const uint8_t*, size_t)`（自实现或 vendored 最小实现，无 OpenSSL 依赖）

**测试用例：**
- 环形缓冲：超过 `max_bytes` 淘汰最旧；`since(ts)` 只返回不早于 `ts` 的块；淘汰计数准确。
- 分段器：`segment_ms=60000`、15fps 时约 60 秒或下一个关键帧处切段；每段以关键帧开头。
- 关键帧起始段的前 4 字节命中 `codec_config` 前缀；非关键帧段不重复前置。
- `flush()` 后的段 `end_ms` 为最后一块时间戳。
- 段 `sha256` 与实际内容一致；内容相同则 sha256 相同（可复现桩数据）。
- `save_atomic` 写入后文件内容可被 `nlohmann/json` 解析；写失败时不留 `.tmp`。

**Steps:**
- [ ] Step 1: 写 `test_recorder.cpp`
- [ ] Step 2: 运行确认失败
- [ ] Step 3: 实现 `recorder.h/cpp` 与 sha256
- [ ] Step 4: `ctest` 全绿
- [ ] Step 5: 提交 `feat(device): ring buffer, keyframe-aligned segmenter and atomic manifest`

---

### Task 7: 事件状态机

**Files:**
- Create: `device/include/hv/events.h`、`device/src/events.cpp`
- Test: `device/tests/test_events.cpp`

**Interfaces:**
- Consumes: `Detection`、`EncodedChunk`、`IClock`、`RingBuffer`、`SegmentInfo`
- Produces:
  - `struct hv::EventRecord`：`event_id`、`EventType type`、`MonotonicMs start_ms`、`std::optional<MonotonicMs> end_ms`、
    `std::optional<double> started_at_unix`、`time_trusted`、`std::vector<Detection> detections`、
    `std::string model_version`、`int config_version`、`ArmMode arm_mode`、`state`（`Active|Closing|Closed|Interrupted`）
  - `class hv::EventManager`：构造接收 `EventPolicy`（确认帧数/持续/阈值、合并窗口、事前/事后时长、分段时长）、
    `RingBuffer&`、`IClock&`；回调 `std::function<void(EventRecord&)> on_open`、`on_chunk(EventRecord&, SegmentInfo)`、
    `on_close(EventRecord&)`；方法 `void on_detection(const Detection&)`、`void on_chunk(const EncodedChunk&)`、
    `void tick(MonotonicMs now)`、`void set_arm_mode(ArmMode, int config_version)`、
    `std::vector<EventRecord> active_events() const`
  - `std::vector<EventRecord> hv::scan_interrupted(spool_dir)`（重启后把未结束事件标记 `Interrupted`）

**测试用例：**
- 单帧命中不触发：需连续 `confirm_frames` 且持续 ≥ `confirm_hold_ms` 才 `on_open`。
- `person_intrusion` 在 `ArmMode::Home` 或 `Unknown` 下**不**触发；切到 `Away` 后触发。
- `fire_suspected`/`smoke_suspected` 在任何布防模式下都触发。
- 触发时 `on_open` 携带事前缓冲的块（`pre_roll_ms` 内），且 `on_chunk` 收到的是完整键帧起始段。
- 静默超过 `post_roll_ms` 后 `tick` 触发 `on_close`，`duration_ms = end_ms - start_ms`。
- 合并窗口内相邻同类事件不产生第二个 `on_open`（归入同一事件）。
- 合并窗口外或不同类事件产生新事件。
- 时钟不可信时 `started_at_unix` 为空、`time_trusted==false`，但 `duration_ms` 正确。
- `scan_interrupted` 把 `Active`/`Closing` 事件标为 `Interrupted`，`Closed` 不变。

**Steps:**
- [ ] Step 1: 写 `test_events.cpp`
- [ ] Step 2: 运行确认失败
- [ ] Step 3: 实现 `events.h/cpp`
- [ ] Step 4: `ctest` 全绿
- [ ] Step 5: 提交 `feat(device): event state machine with debounce, merge and roll windows`

---

### Task 8: 落盘与配额

**Files:**
- Create: `device/include/hv/spool.h`、`device/src/spool.cpp`
- Test: `device/tests/test_spool.cpp`

**Interfaces:**
- Consumes: `Manifest`、`SegmentInfo`、`EventRecord`、`Result`
- Produces:
  - `class hv::Spool`：`Spool(spool_dir, QuotaConfig)`、
    `Result create_event(const EventRecord&, const EncoderConfig&)`（建目录并原子写清单）、
    `Result append_segment(const std::string& event_id, const uint8_t* data, size_t, SegmentInfo&)`（先临时文件再 rename，随后原子更新清单）、
    `Result write_snapshot(event_id, const std::vector<uint8_t>&, std::string content_type)`、
    `Result mark_uploaded(event_id, int seq)`、`Result mark_meta_uploaded(event_id)`、`Result mark_snapshot_uploaded(event_id)`、
    `Result mark_closed(event_id, end_ms, std::optional<double> ended_at, int64_t duration_ms, bool time_trusted)`、
    `std::vector<PendingItem> pending(size_t limit)`（按“元数据 → 截图 → 片段 → 关闭”排序）、
    `std::vector<std::string> events_in_state(State)`、`Result remove_event(event_id)`、
    `QuotaReport enforce_quotas()`、`std::vector<std::string> restore_active_events()`
  - `struct PendingItem { enum Kind { Meta, Snapshot, Segment, Close } kind; std::string event_id; int seq; std::filesystem::path path; std::string sha256; uint64_t bytes; }`
  - `struct QuotaReport { uint64_t evicted_bytes, evicted_events; std::vector<std::string> evicted_ids; }`

**测试用例：**
- `create_event` 后目录与 `manifest.json` 存在且可解析；重复调用不覆盖已有片段。
- `append_segment` 后清单中段序、字节、sha256 正确且文件可读回；写失败（目标目录只读）返回非 Ok 且不留临时文件。
- `pending` 顺序固定为元数据 → 截图 → 片段（按 seq 升序）→ 关闭。
- 只有 `mark_uploaded` 后对应项才从 `pending` 消失；未确认项始终保留。
- 配额：写入超过“已上传桶”上限时只淘汰已上传且已关闭的事件；**待上传的 `fire_suspected` 事件在普通待上传事件之后才被淘汰**。
- 正在上传（存在上传中标记）的事件不被淘汰。
- 淘汰后 `QuotaReport` 与 `lost.json` 计数一致。
- `restore_active_events` 返回重启前未关闭的事件 id。

**Steps:**
- [ ] Step 1: 写 `test_spool.cpp`
- [ ] Step 2: 运行确认失败
- [ ] Step 3: 实现 `spool.h/cpp`
- [ ] Step 4: `ctest` 全绿
- [ ] Step 5: 提交 `feat(device): durable spool with atomic manifests and quota eviction`

---

### Task 9: 集线器协议客户端

**Files:**
- Create: `device/include/hv/hub_client.h`、`device/src/hub_client.cpp`
- Test: `device/tests/test_hub_client.cpp`（用 `httplib::Server` 起本地桩集线器）

**Interfaces:**
- Consumes: `httplib.h`、`json.hpp`、`Result`
- Produces:
  - `struct hv::HeartbeatResult { Result r; std::optional<int64_t> server_unix; }`
  - `struct hv::EventUpsertResult { Result r; bool created; }`
  - `enum class hv::UploadOutcome { Ok, Duplicate, Conflict, EventMissing, ChecksumMismatch, InsufficientStorage, Invalid, NetworkError, ServerError }`
  - `class hv::HubClient`：`HubClient(std::string base_url, std::string device_id, int timeout_ms)`、
    `HeartbeatResult heartbeat(const DeviceReport&)`、`Result get_arm(ArmMode&, int& version)`、
    `Result ack_arm(ArmMode applied, int version)`、`EventUpsertResult create_event(const json& metadata)`、
    `UploadOutcome upload_snapshot(event_id, path, content_type)`、
    `UploadOutcome upload_segment(event_id, int seq, path, sha256_hex)`、
    `Result close_event(event_id, const json& close_payload)`、
    `Result cleanup()`
  - `json hv::to_json(const DeviceReport&)`（字段与集线器 `DeviceReport` 逐字一致）
  - `std::optional<int64_t> hv::parse_http_date(const std::string&)`

**测试用例：**
- 心跳请求体字段与集线器 `DeviceReport` 逐字一致（键集合断言，含 `device_id`、`boot_id`、`uptime_ms`、`last_frame_ms`、`last_encode_ms`、`person/fire/smoke_last_detection_ms`、`video_ok`、`recording_ok`、`storage_free_bytes`、`arm_applied`、`arm_applied_version`、`time_trusted`）。
- 从未触发的检测器：`*_last_detection_ms` 为 JSON `null`，不是 `0`。
- 心跳从响应的 `Date` 头解析出 `server_unix`；无 `Date` 头时为 `nullopt`。
- `GET /arm` 解析 `{mode, config_version}`；`mode` 为 `unknown` 时返回 `ArmMode::Unknown` 且版本 0。
- `create_event`：201 → `created=true`；200 → `created=false`；422 → `Code::Invalid`。
- `upload_snapshot`：201 → `Ok`；200（`status:"duplicate"`）→ `Duplicate`；404 → `EventMissing`；507 → `InsufficientStorage`。
- `upload_segment`：201 → `Ok`；200 → `Duplicate`；409 → `Conflict`；400 → `ChecksumMismatch`；并断言请求带 `X-Segment-Sha256` 头。
- 连接被拒（指向未监听端口）→ `NetworkError`，不抛异常。
- `ack_arm` 发送 `{device_id, applied_mode, applied_version}`；`get_arm` 后 `effective` 由集线器决定，客户端不推断。

**Steps:**
- [ ] Step 1: 写 `test_hub_client.cpp`（本地 `httplib::Server` 桩）
- [ ] Step 2: 运行确认失败
- [ ] Step 3: 实现 `hub_client.h/cpp`
- [ ] Step 4: `ctest` 全绿
- [ ] Step 5: 提交 `feat(device): hub protocol client with heartbeat, arm and media upload`

---

### Task 10: 上传队列与退避

**Files:**
- Create: `device/include/hv/uploader.h`、`device/src/uploader.cpp`
- Test: `device/tests/test_uploader.cpp`

**Interfaces:**
- Consumes: `Spool`、`HubClient`、`IClock`
- Produces:
  - `struct hv::BackoffPolicy { int64_t base_ms, max_ms; int max_attempts; double jitter; }`、
    `int64_t hv::backoff_delay_ms(const BackoffPolicy&, int attempt, uint64_t rng)`
  - `struct hv::UploadStats { uint64_t meta_ok, snapshot_ok, segment_ok, retries, duplicates, conflicts, permanent_failures; uint64_t bytes_sent; }`
  - `class hv::Uploader`：`Uploader(Spool&, HubClient&, IClock&, BackoffPolicy, uint64_t rate_bytes_per_s)`、
    `void run_for(MonotonicMs budget_ms)`、`UploadStats stats() const`、`void request_stop()`

**测试用例：**
- `backoff_delay_ms` 单调上升且不超过 `max_ms`；同样 `rng` 下可复现；`jitter=0` 时精确等于 `base * 2^attempt`。
- 顺序：同一事件的元数据先于截图、截图先于任何片段、片段按 seq、关闭最后。
- 网络失败重试至 `max_attempts` 后计入 `permanent_failures`，且该事件其余项不被无限重试风暴阻塞（下一事件仍被处理）。
- 200/`duplicate` 计入 `duplicates` 并**视为成功**（该项被标记为已上传）。
- 409 `Conflict` 不重试（计入 `conflicts`），且不阻塞其余项。
- 404 `EventMissing` 时先把该事件元数据重新入队；元数据成功后片段可继续。
- 507 `InsufficientStorage` 退避重试，不丢弃本地文件。
- 限速：`rate_bytes_per_s` 生效，`run_for` 预算用满后即返回，不空转。
- `request_stop()` 后 `run_for` 立即返回且未确认项保留。

**Steps:**
- [ ] Step 1: 写 `test_uploader.cpp`
- [ ] Step 2: 运行确认失败
- [ ] Step 3: 实现 `uploader.h/cpp`
- [ ] Step 4: `ctest` 全绿
- [ ] Step 5: 提交 `feat(device): rate-limited upload queue with capped backoff`

---

### Task 11: 健康上报与布防控制

**Files:**
- Create: `device/include/hv/health.h`、`device/src/health.cpp`
- Test: `device/tests/test_health.cpp`

**Interfaces:**
- Consumes: `IClock`、`HubClient`、`Spool`、队列统计
- Produces:
  - `class hv::HealthMonitor`：`void note_frame(MonotonicMs)`、`note_encode(MonotonicMs)`、
    `note_detection(EventType, MonotonicMs)`、`note_recording_ok(bool)`、`note_storage_free(uint64_t)`、
    `DeviceReport build(MonotonicMs now, const HealthThresholds&) const`
  - `class hv::ArmController`：`ArmController(HubClient&, IClock&, state_dir, std::string device_id)`、
    `Result poll_and_apply(MonotonicMs now)`（拉取 → 应用 → 持久化 → `ack`）、
    `ArmMode applied_mode() const`、`int applied_version() const`、`void load_persisted()`、
    `bool should_run_person_detection() const`（仅 `Away`）
  - `hv::HealthThresholds { MonotonicMs video_stale_ms, recording_stale_ms; }`

**测试用例：**
- 从未 `note_*` 时：`person/fire/smoke_last_detection_ms` 均为 `nullopt`（→ JSON `null`），`video_ok`/`recording_ok` 为 `nullopt`（→ `null`），不是 `false`。
- `video_ok==false` 仅在最后编码成功时间超过 `video_stale_ms` 时给出；未编码过则 `nullopt`。
- `uptime_ms` 与传入 `now` 一致（单调）。
- 布防：`GET /arm` 返回版本大于本地时应用并 `ack`；版本相同不重复 `ack`。
- `unknown` 模式只在从未确认过时出现；重启后 `load_persisted` 恢复最后确认的模式与版本。
- `should_run_person_detection()` 仅在 `Away` 为 true。
- 拉取失败（网络错误）时保留上次模式，返回非 Ok，且不发出 `ack`。

**Steps:**
- [ ] Step 1: 写 `test_health.cpp`
- [ ] Step 2: 运行确认失败
- [ ] Step 3: 实现 `health.h/cpp`
- [ ] Step 4: `ctest` 全绿
- [ ] Step 5: 提交 `feat(device): health reporting and arm apply/ack control`

---

### Task 12: 装配、主循环与假设备模式

**Files:**
- Create: `device/include/hv/app.h`、`device/src/app.cpp`、`device/src/main.cpp`、`device/src/pipeline.cpp`
- Create: `device/include/hv/pipeline.h`、`device/include/hv/supervisor.h`、`device/src/supervisor.cpp`
- Test: `device/tests/test_app.cpp`

**Interfaces:**
- Consumes: 前述全部模块
- Produces:
  - `class hv::Pipeline`：持有摄像头、编码器、三路检测器、四个线程与队列；`start()`、`stop()`；
    `PipelineStats stats()`（各级队列统计、帧/块/检测计数、最后成功时间）
  - `class hv::Supervisor`：`void check(MonotonicMs now)`，依据最后成功时间对编码/检测发起重建；
    `std::vector<RecoveryAction> history() const`；连续重启计数上限可配
  - `class hv::App`：`App(DeviceConfig, std::unique_ptr<IClock>)`、`int run(int argc, char** argv)`；
    支持 `--config PATH`、`--fake-device`、`--once`（跑一个短回合后退出，供联调脚本用）
- 假设备模式：合成摄像头 + 桩编码器 + **可脚本化桩检测器**（由配置文件/命令行给出“在第 N 秒触发 person/fire 一次”），
  用于无硬件联调。

**测试用例：**
- `App` 用假时钟跑一个短回合：产生心跳、创建事件、写清单、上传（对桩集线器）、关闭事件，全部无异常退出。
- `--once` 在有限时间内返回 0。
- `Supervisor`：编码超过 `video_stale_ms` 未产出时请求一次重建；重建后恢复正常则不再请求。
- 连续重建次数达上限后停止请求并记录（不无限重启）。
- 停止：`stop()` 后所有线程 join，无泄漏（ASan/断言式检查：队列均已 `close`）。

**Steps:**
- [ ] Step 1: 写 `test_app.cpp`
- [ ] Step 2: 运行确认失败
- [ ] Step 3: 实现 `pipeline.h/cpp`、`supervisor.h/cpp`、`app.h/cpp`、`main.cpp`
- [ ] Step 4: `ctest` 全绿
- [ ] Step 5: 提交 `feat(device): pipeline wiring, main loop and fake device mode`

---

### Task 13: 集线器端到端联调与文档

**Files:**
- Create: `device/tools/e2e_with_hub.sh`
- Create: `device/README.md`
- Create: `device/config/example.json`
- Modify: `device/CMakeLists.txt`（把联调注册为可选 CTest，缺 Python 时跳过）

**Interfaces:**
- Consumes: 真实集线器（`uvicorn homevideo_hub.api:create_app --factory`）+ `hv_device --fake-device --once`
- Produces: 可重复的联调脚本与设备端 README（运行方法、配置项、目录结构、待实机项、已知缺口）

**测试用例（脚本断言）：**
- 集线器数据目录中事件数 ≥1，且 `GET /api/v1/events/{id}` 返回 `snapshot` 非空、`segments` 至少 1 段。
- 重复上传同一事件不产生第二行（再跑一次 `--once`，事件总数不变）。
- 关闭后事件 `status=="closed"` 且 `duration_ms` 存在。
- 心跳使 `GET /api/v1/devices/{id}/health` 的 `link=="online"`。
- 布防：`PUT /api/v1/arm` 为 `away` 后设备 `ack`，`effective==true`。

**Steps:**
- [ ] Step 1: 写 `e2e_with_hub.sh`（启动集线器 → 跑假设备 → 用 curl 断言 → 清理）
- [ ] Step 2: 运行脚本，确认当前失败/不通过
- [ ] Step 3: 修正接线与默认配置，直到脚本全绿
- [ ] Step 4: 写 `device/README.md` 与 `config/example.json`；把脚本注册为可选 CTest
- [ ] Step 5: 提交 `test(device): end-to-end run against the real hub`

---

## 验证方式

```bash
# 单元测试（无需硬件、无需集线器）
cmake -S device -B build/device && cmake --build build/device -j
ctest --test-dir build/device --output-on-failure

# 设备 ↔ 集线器联调（需要仓库根的 .venv）
device/tools/e2e_with_hub.sh

# 交叉编译（阶段 A 实测通过后）
cmake -S device -B build/v831 -DCMAKE_TOOLCHAIN_FILE=device/cmake/toolchain-v831.cmake
```

## 后续计划

1. **阶段 A 实测**：镜像、摄像头采集、MPP 硬件编码、模型转换。产出决定 `platform/v831_*` 的实现。
2. **补齐 v831 后端**：真实采集、硬件 H.264、NPU 推理；用同一份 `ctest` 与联调脚本回归。
3. **子系统 A2**：设备输出压缩流后接入 RTSP → go2rtc → Tailscale → 手机。
4. **子系统 C**：基于集线器 API 的手机网页。
5. **子系统 D**：板卡能力验证记录表。
6. **认证**：集线器与设备目前均无认证（见 `hub/README.md` 已知缺口）；在 A2 落地时统一加入。
