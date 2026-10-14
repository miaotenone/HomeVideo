# HomeVideo 设备端（V831）软件设计

**范围：** 方案 `docs/homevideo-design.md` 的**子系统 B**——运行在 V831 上的长驻设备程序，
负责采集、编码、检测、事件录像，并与树莓派集线器（子系统 A）对接。

**状态：** 首批实现“不依赖硬件的骨架”：结构、接口、纯逻辑与设备↔集线器联调。
摄像头、硬件编码、NPU 推理三个后端先以桩实现占位，待 §8 阶段 A 实测结果出来后补齐。

**依据：** 方案 §4（V831 软件方案）、§5（事件录像与存储）、§7（事件通信与异常恢复）、
§8 阶段 A；集线器协议见 `hub/README.md` 与 `hub/tests/test_end_to_end.py`。

---

## 1. 本批做什么、不做什么

**做**（全部可在开发机上构建、运行和验证）：

- 工程骨架与构建系统，含开发机与 V831 两套构建目标。
- 采集 → 编码 → 录制的管线结构、有界队列与溢出策略。
- 事件生命周期：事前缓冲、触发确认、同类合并、结束后补录、分段、清单。
- 落盘与配额（滚动缓冲、待上传、已上传、元数据）。
- 与集线器的完整协议客户端，以及上传队列（限速、退避、幂等、断网补传）。
- 健康上报、布防拉取与确认、时间可信判定。
- 桌面后端（合成画面、桩编码器、桩检测器），以及面向**真实集线器**的端到端联调。

**不做**（待实机或属其他子系统）：

- 真实摄像头采集、MPP 硬件 H.264 编码、NPU 模型推理——只留接口与桩。
- 实时视频转发（RTSP → go2rtc → 手机），属子系统 A2。
- 树莓派侧 MP4 重封装。
- 看门狗与开机自启脚本的实机验证。

## 2. 技术选型

| 选择 | 取值 | 理由 |
| --- | --- | --- |
| 语言与标准 | C++17 | 方案 §4.1：长期运行程序用 C/C++ 与板卡 SDK，便于控制内存与硬件媒体接口 |
| 构建 | CMake ≥ 3.16 | libmaix 的桌面平台同样可用；交叉编译用工具链文件 |
| 线程模型 | 每级一个线程 + 有界队列 | 不引入异步框架；环节阻塞可在健康报告里显式表达 |
| HTTP/JSON | vendored `cpp-httplib`、`nlohmann/json` | 两者都是 header-only；musl 交叉环境没有包管理器，无构建依赖最省事 |
| 测试 | 自带极简单头测试框架 + CTest | 不引入第三方测试依赖，交叉环境也能编 |
| 交叉工具链 | `arm-openwrt-linux-muslgnueabi-` | 方案 §4.1 已冻结 |

依赖上限：不引入数据库服务、消息队列、转码组件；设备端不常驻解码。

## 3. 目录结构

```
device/
  CMakeLists.txt
  cmake/toolchain-v831.cmake       # 交叉编译工具链文件
  third_party/
    httplib.h                      # vendored，MIT
    json.hpp                       # vendored，MIT
  include/hv/
    types.h                        # 公共类型：时钟量、事件类型、帧、检测结果
    config.h                       # DeviceConfig 与加载
    clock.h                        # IClock、TimeSource（时间可信判定）
    log.h
    status.h                       # 错误码与结果类型
    queue.h                        # BoundedQueue
    platform.h                     # ICameraSource、IEncoder、IDetector 接口
    pipeline.h                     # 管线装配与统计
    recorder.h                     # 环形缓冲、分段器、清单
    events.h                       # EventManager 状态机
    spool.h                        # 持久落盘与配额
    hub_client.h                   # 集线器协议客户端
    uploader.h                     # 上传队列与退避
    health.h                       # 健康监视与上报、布防控制
    app.h                          # 装配与主循环
  src/
    config.cpp clock.cpp log.cpp queue.cpp
    platform/desktop_*.cpp         # 合成摄像头、桩编码器、桩检测器
    platform/v831_*.cpp            # 待阶段 A：MPP/libmaix/NPU
    pipeline.cpp recorder.cpp events.cpp spool.cpp
    hub_client.cpp uploader.cpp health.cpp app.cpp main.cpp
  tests/
    harness.h                      # 极简测试框架
    test_*.cpp
  tools/
    e2e_with_hub.sh                # 与真实集线器的联调脚本
  README.md
```

单一职责：`platform/` 是唯一接触硬件的目录；`events.cpp` 只做状态机；`spool.cpp` 只做
持久化与配额；`hub_client.cpp` 只做协议映射，不含重试策略（重试在 `uploader.cpp`）。

## 4. 硬件接缝（接口）

只有三个接口直接触碰硬件。三者都有桌面桩实现，桩之外的部分不感知硬件。

```cpp
struct Frame {
  MonotonicMs ts_ms;            // 单调时钟，自进程启动
  uint32_t width, height;
  PixelFormat format;           // Yuv420 | Rgb888
  const uint8_t* data; size_t size;   // 归采集源所有，下一次 read() 前有效
};

class ICameraSource {
  virtual bool open();
  virtual bool read(Frame& out, int timeout_ms);   // false = 超时/错误
  virtual void close();
  virtual std::string last_error() const;
};

class IEncoder {
  virtual bool open(const EncoderConfig&);
  virtual bool encode(const Frame&, EncodedChunk& out);   // 可能产出 0 或 1 块
  virtual const std::vector<uint8_t>& codec_config() const;  // SPS/PPS
  virtual void request_keyframe();
  virtual void close();
};

class IDetector {          // 每个标签一个实例：person / fire / smoke
  virtual bool load(const DetectorConfig&);   // 模型路径、输入尺寸、阈值
  virtual bool run(const Frame&, std::vector<Detection>& out);
  virtual std::string model_version() const;
  virtual void unload();
};
```

**契约（后端必须满足）：**

- `ICameraSource::read` 在同一采集源上可被编码路径与检测路径共享；若硬件只给一路，
  后端内部负责缩放/抽帧，不外泄给上层。检测输入分辨率由后端决定。
- `IEncoder::codec_config()` 在关键帧前必须可用；分段器会把该配置前置到每个关键帧起始段，
  保证片段自描述、可供集线器仅重封装不解码（方案 §5.2）。
- `IEncoder::encode` 不得阻塞超过配置上限；编码器重启由上层按健康监视触发。
- `IDetector::run` 不得持有帧所有权；输出置信度需与模型实际输出一致。

## 5. 线程与队列

| 线程 | 职责 | 下游队列策略 |
| --- | --- | --- |
| capture/encode | 读帧 → 编码 → 产出 `EncodedChunk` | 交给 record 与 detect 两路 |
| record | 环形缓冲 → 事件引用 → 分段落盘 | **不静默丢弃**；丢记 `recording_ok=false` |
| detect | 按固定间隔轮询三个检测器 | 丢弃过期帧，只取最新帧 |
| upload | 待上传队列：元数据/截图/片段/关闭 | 限速 + 退避 |

- 队列都有界：容量按字节或条数配置。`OverflowPolicy` 为 `DropOldest | DropNewest | Reject`。
- **录制路径不允许静默丢帧**（方案 §4.2 的丢弃规则针对实时观看，不针对录像）。录制队列溢出
  记为硬错误信号并进入健康报告。
- 检测路径刻意丢旧帧：检测只需最新画面，堆积只会增加延迟。
- 队列统计（入队/出队/丢弃/当前深度/字节）进入健康报告与日志。

## 6. 事件生命周期

```
idle ──触发确认通过──▶ active ──静默超过 post_roll──▶ closing ──▶ closed
  ▲                     │                                          │
  └────── 合并窗口内同类事件归入同一事件 ───────────────────────────────┘
                        └── 进程异常退出 → interrupted（重启后标记）
```

- **触发确认**：连续 N 帧命中、持续 ≥ `confirm_hold_ms`、置信度 ≥ 阈值。
  `person_intrusion` 还需当前已确认布防为 `away`；`fire`/`smoke` 常开（方案 §4.4）。
- **去抖**：火灾与烟雾同样需要确认窗口，避免单帧误报。
- **合并**：相邻同类事件在 `merge_window_ms` 内合并为同一事件，限制重复通知（方案 §4.3）。
- **事前缓冲**：环形缓冲按单调时间保留 `pre_roll_ms`（默认 10s）的已编码块。
- **结束后补录**：关闭前继续录制 `post_roll_ms`（默认 20s）。
- **分段**：约 `segment_ms`（默认 60s）一段，从关键帧开始，归属同一事件。
- 事件打开时**先**上传元数据与截图，不等录像完成（方案 §4.4、§7.1）。

## 7. 落盘格式与配额

```
spool/
  events/{event_id}/manifest.json     # 事件元数据 + 编码参数 + 片段清单与上传状态
  events/{event_id}/snapshot.jpg
  events/{event_id}/seg_0000.bin
  state/device_state.json             # boot_id、已确认布防模式与版本、时间基准
  lost.json                           # 丢失计数（事件/字节）
```

`manifest.json`（原子替换写入，先写临时文件再 rename）：

```json
{
  "manifest_version": 1,
  "event": {
    "event_id": "v831a-3f9c1d2e-173456-0007",
    "device_id": "v831a",
    "event_type": "person_intrusion",
    "started_at": 1760000000.0,
    "time_trusted": true,
    "device_boot_id": "3f9c1d2e",
    "device_started_ms": 173456,
    "model_version": "person-yolo-v1",
    "config_version": 3,
    "arm_mode": "away",
    "detections": [
      {"label": "person_intrusion", "confidence": 0.82, "bbox": [10, 20, 30, 40], "zone": "door"}
    ]
  },
  "codec": {"format": "h264", "width": 1280, "height": 720,
            "fps": 15, "bitrate_bps": 1500000, "gop_ms": 2000},
  "segments": [
    {"seq": 0, "file": "seg_0000.bin", "bytes": 123456, "sha256": "...",
     "keyframe": true, "start_ms": 1000, "end_ms": 61000, "uploaded": true}
  ],
  "state": "closed",
  "closed": {"ended_at": 1760000075.0, "duration_ms": 75000, "time_trusted": true}
}
```

这份清单同时满足方案 §7.1 要求的“视频片段列表和上传状态”，也是将来集线器侧重封装所需的
编码参数与时间信息来源（集线器目前没有清单端点，清单留在设备侧）。

**配额**（方案 §5.3）分四个桶：滚动缓冲、待上传事件、已上传事件、元数据与日志。
达到上限时按顺序清理：滚动缓冲 → 已上传事件 → 待上传的普通事件 → **待上传的疑似火灾事件最后**。
每次淘汰写 `lost.json` 计数。正在写入、正在上传或被未完成事件引用的文件不清理。

## 8. 与集线器的协议对应

| 设备动作 | 端点 | 关键语义 |
| --- | --- | --- |
| 心跳 | `POST /device/heartbeat` | 默认 10s；返回体无时间字段，时间取响应的 HTTP `Date` 头 |
| 拉布防 | `GET /arm` | 默认 5s；`config_version` 增大才应用 |
| 确认布防 | `POST /arm/ack` | 应用并持久化之后才确认；`unknown` 不得主动上报为已布防 |
| 事件元数据 | `POST /events` | 201 新建 / 200 已存在（幂等，可重复） |
| 截图 | `POST /events/{id}/snapshot` | `Content-Type: image/jpeg`；200 表示已有截图，视为成功 |
| 片段 | `PUT /events/{id}/segments/{seq}` | 头 `X-Segment-Sha256`；200 同校验值＝幂等成功；409 冲突＝本地不一致，记录后停止该段重试；404 事件未达，先补元数据；400 校验不符＝本地文件损坏，标记损坏 |
| 关闭 | `POST /events/{id}/close` | 幂等；时钟不可信时 `started_at`/`ended_at` 存 NULL，`duration_ms` 始终记录 |
| 清理 | 本地 | 只有集线器确认后才清理设备副本 |

**顺序**：元数据 → 截图 → 片段 → 关闭（方案 §7.1）。**退避**：指数 + 抖动，上限默认 60s，
最大尝试次数可配；网络故障时不得空转占 CPU。**限速**：片段上传单独限速，避免占满带宽影响通知。

**身份**：集线器当前没有认证，设备端以普通 HTTP 访问，且只应指向局域网或 Tailscale 地址。
认证接口预留在一个函数内（`HubClient` 统一加头），待 A2 落地后接上。

## 9. 健康、布防与时间

- 心跳字段与集线器 `DeviceReport` 逐字对齐：`boot_id`、`uptime_ms`、`last_frame_ms`、
  `last_encode_ms`、`{person,fire,smoke}_last_detection_ms`、`video_ok`、`recording_ok`、
  `storage_free_bytes`、`arm_applied`、`arm_applied_version`、`time_trusted`。
- 全部 `*_ms` 使用单调时钟（方案 §7.2：系统时间校正不得影响时长判断）。
- 检测陈旧由集线器按 5s 判定，因此调度器必须保证**每个标签**的完成间隔 < 5s；人员、火灾、烟雾
  三者都要有独立 deadline，不能让其中一个长期占用推理资源（方案 §4.4）。
- `video_ok` / `recording_ok` 由“最后一次成功”的时长派生，阈值可配。
- 时间可信：以集线器响应的 `Date` 头为准，与本地墙钟偏差超阈值即标记不可信；不可信时
  `time_trusted=false` 且不写 `started_at`/`ended_at`。
- 断网时继续执行最后一次确认的布防配置，火灾检测不受影响。

## 10. 故障恢复

按方案 §7.3：先模块重建、再进程重启、最后看门狗。本批实现：

- 每个环节记录“最后一次成功时间”，进入健康报告。
- `Supervisor` 检测停滞（超过实测正常最大耗时）时请求该级重建；编码器提供
  `request_keyframe()` 与重建路径。
- 进程级重启由外部进程管理器负责，提供 systemd 单元样例但不在本批验证。
- 重启后：已完成片段保留，未正常结束的事件标记为 `interrupted`（方案 §5.3）。

## 11. 构建与测试

```bash
# 开发机
cmake -S device -B build/device -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/device -j
ctest --test-dir build/device --output-on-failure

# V831（阶段 A 实测通过后）
cmake -S device -B build/v831 -DCMAKE_TOOLCHAIN_FILE=device/cmake/toolchain-v831.cmake

# 与真实集线器的联调
device/tools/e2e_with_hub.sh
```

联调脚本启动真实集线器（`uvicorn homevideo_hub.api:create_app --factory`）+ 设备端
`--fake-device` 模式（合成画面 + 桩编码器 + 可编排的桩检测器），然后用集线器 HTTP 接口断言：
元数据、截图、片段、关闭全部到达，且重复上传不产生重复事件。

## 12. 待实机确认（阶段 A 依赖）

| 待确认 | 影响的本模块 |
| --- | --- |
| 摄像头是否支持编码与检测共享采集，或需 SDK 缩放/抽帧 | `platform/v831_camera.cpp`、`pipeline.cpp` |
| MPP 硬件 H.264 是否可用、头文件从哪来 | `platform/v831_encoder.cpp`、`codec_config()` 内容 |
| 硬件编码的实际关键帧间隔与可控性 | `recorder.cpp` 分段对齐 |
| NPU 运行时、模型转换与 MUD 模型版本 | `platform/v831_detector.cpp` |
| 三个模型同时驻留的内存开销 | 检测调度间隔、是否轮换加载 |
| 64MB 内存下的编码+检测并行余量 | 队列容量、事前缓冲是内存还是 SD |

## 13. 明确不做（anti-goals）

- 不在设备端做 MP4 封装、转码或解码。
- 不长期保存普通视频：滚动缓冲只保留事件前窗口。
- 不引入数据库服务、消息队列、容器。
- 不把“视觉疑似火灾”表述为已确认火灾；事件类型字面量固定为 `fire_suspected`。
- 不在设备端实现面向手机的服务：手机只连树莓派。
