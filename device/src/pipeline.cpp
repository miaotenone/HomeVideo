#include "hv/pipeline.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "hv/log.h"
#include "hv/platform.h"
#include "hv/queue.h"

// Three threads and bounded queues (design §4.2/§5): capture/encode, record and
// detect. The camera is never blocked -- the chunk queue drops its oldest entry
// and the detect path keeps only the newest frame -- while a lost encoded chunk
// is a reportable recording failure (§7.3), never a silent drop. The per-label
// round robin keeps every label's completion gap under the hub's 5s stale
// threshold (§9).

namespace hv {

namespace {

// The record path must not silently lose data, so the queue is small and
// drained promptly. 8 is tiny next to a producer that never sleeps, which is
// what the drop test relies on; a slow consumer makes it overflow.
constexpr size_t kChunkQueueCapacity = 8;

// Bounded waits keep stop() responsive even if a backend stalls mid-call.
constexpr int kCameraReadTimeoutMs = 100;
constexpr int kQueuePopTimeoutMs = 50;

}  // namespace

struct Pipeline::Impl {
  Impl(const DeviceConfig& config, IClock& clock, Hooks hooks)
      : config_(config), clock_(clock), hooks_(std::move(hooks)) {
    // std::atomic is not value-initialised before C++20; the "never" sentinel
    // (-1) and the counters must be set explicitly.
    for (auto& runs : detection_runs_by_label_) runs.store(0);
    for (auto& at : last_detection_ms_) at.store(-1);
    for (auto& detector : detectors_) detector = nullptr;
  }

  ~Impl() { stop(); }

  // --- lifecycle ---------------------------------------------------------

  Result start() {
    if (started_) return Result::success();
    stopping_.store(false);
    recording_ok_.store(true);
    logged_recording_failure_ = false;
    next_slot_ = 0;
    detect_seen_seq_ = 0;
    {
      std::lock_guard<std::mutex> lock(mailbox_mutex_);
      mailbox_ready_ = false;
      mailbox_slot_.reset();
      mailbox_seq_ = 0;
      mailbox_overwrites_ = 0;
    }

    // A fresh queue each start(): close() is permanent, and stop() always closes.
    chunk_queue_ = std::make_unique<BoundedQueue<EncodedChunk>>(
        kChunkQueueCapacity, OverflowPolicy::DropOldest);

    CameraOptions camera_options;
    camera_options.width = config_.capture.width;
    camera_options.height = config_.capture.height;
    camera_options.fps = config_.capture.fps;
    camera_options.realtime = config_.capture.realtime;
    // Frames must carry the device's monotonic base (§9), not the camera's own
    // steady clock.
    camera_options.clock = &clock_;

    camera_ = create_camera(camera_options);
    if (!camera_) {
      return unwind(Result::error(Code::Internal, "camera: no backend"));
    }
    const Result opened_camera = camera_->open();
    if (!opened_camera.ok()) {
      return unwind(Result::error(opened_camera.code(),
                                  "camera: " + camera_->last_error()));
    }

    encoder_config_ = EncoderConfig{};
    encoder_config_.codec = config_.encode.codec;
    encoder_config_.width = config_.capture.width;
    encoder_config_.height = config_.capture.height;
    encoder_config_.fps = config_.capture.fps;
    encoder_config_.bitrate_bps = config_.encode.bitrate_bps;
    encoder_config_.gop_ms = config_.encode.gop_ms;

    encoder_ = create_encoder();
    if (!encoder_) {
      return unwind(Result::error(Code::Internal, "encoder: no backend"));
    }
    const Result opened_encoder = encoder_->open(encoder_config_);
    if (!opened_encoder.ok()) {
      return unwind(Result::error(opened_encoder.code(),
                                  "encoder: " + encoder_->last_error()));
    }

    detector_configs_ = make_detector_configs(config_);
    for (const DetectorConfig& detector_config : detector_configs_) {
      const size_t slot = index_of(detector_config.label);
      if (detectors_[slot]) continue;  // duplicate label: first one wins
      auto detector = create_detector();
      if (!detector) {
        return unwind(Result::error(
            Code::Internal,
            std::string("detector[") + to_wire(detector_config.label) +
                "]: no backend"));
      }
      const Result loaded = detector->load(detector_config);
      if (!loaded.ok()) {
        return unwind(Result::error(
            loaded.code(),
            std::string("detector[") + to_wire(detector_config.label) +
                "]: " + detector->last_error()));
      }
      detectors_[slot] = std::move(detector);
    }

    // Detectors are loaded once here; arming only toggles a scheduler slot, so
    // set_person_detection_enabled never touches a model.
    capture_thread_ = std::thread([this] { capture_loop(); });
    record_thread_ = std::thread([this] { record_loop(); });
    detect_thread_ = std::thread([this] { detect_loop(); });
    started_ = true;
    HV_INFO("pipeline started " << config_.capture.width << "x"
                                << config_.capture.height << "@"
                                << config_.capture.fps);
    return Result::success();
  }

  void stop() {
    stopping_.store(true);
    // Wake the detect thread out of either wait before joining it.
    mailbox_cv_.notify_all();
    pace_cv_.notify_all();

    // Join capture first: once it is gone nothing can push, so the record
    // thread can drain and exit instead of racing the closure.
    if (capture_thread_.joinable()) capture_thread_.join();
    if (chunk_queue_) chunk_queue_->close();
    if (record_thread_.joinable()) record_thread_.join();
    if (detect_thread_.joinable()) detect_thread_.join();

    {
      // Leave no pending frame behind: stop() must report depth 0.
      std::lock_guard<std::mutex> lock(mailbox_mutex_);
      mailbox_ready_ = false;
      mailbox_slot_.reset();
    }

    if (encoder_) {
      std::lock_guard<std::mutex> lock(encoder_mutex_);
      encoder_->close();
      encoder_.reset();
    }
    if (camera_) {
      camera_->close();
      camera_.reset();
    }
    for (auto& detector : detectors_) {
      if (detector) {
        detector->unload();
        detector.reset();
      }
    }
    started_ = false;
  }

  // --- threads -----------------------------------------------------------

  /** Capture + encode: the only thread that owns the camera and the encoder. */
  void capture_loop() {
    while (!stopping_.load()) {
      Frame frame;
      if (!camera_->read(frame, kCameraReadTimeoutMs)) continue;  // timeout/error

      EncodedChunk chunk;
      bool encoded = false;
      {
        // restart_encoder() rebuilds in place from the Supervisor thread.
        std::lock_guard<std::mutex> lock(encoder_mutex_);
        encoded = encoder_ && encoder_->encode(frame, chunk);
      }
      if (encoded) {
        // Video freshness is the frame that made it through the encoder, so
        // both stamps come from the chunk's monotonic timestamp.
        frames_.fetch_add(1);
        last_frame_ms_.store(chunk.ts_ms);
        chunks_.fetch_add(1);
        if (chunk.keyframe) keyframes_.fetch_add(1);
        bytes_encoded_.fetch_add(static_cast<uint64_t>(chunk.data.size()));
        last_encode_ms_.store(chunk.ts_ms);
        if (chunk_queue_) {
          // DropOldest: making room for the newest chunk is the only way to
          // keep the camera running; the loss is visible as recording_ok=false.
          chunk_queue_->push(std::move(chunk));
        }
      }

      // Publish after encoding so the copy happens before the camera can reuse
      // its buffer on the next read().
      publish_frame(frame);
    }
  }

  /** Record: drain encoded chunks and notice any drop the queue had to make. */
  void record_loop() {
    uint64_t seen_dropped = 0;
    uint64_t seen_rejected = 0;
    while (true) {
      EncodedChunk chunk;
      const bool popped = chunk_queue_ && chunk_queue_->pop(chunk, kQueuePopTimeoutMs);
      if (popped && hooks_.on_chunk) hooks_.on_chunk(chunk, chunk.ts_ms);

      if (chunk_queue_) {
        const BoundedQueue<EncodedChunk>::Stats queue_stats = chunk_queue_->stats();
        if (queue_stats.dropped != seen_dropped ||
            queue_stats.rejected != seen_rejected) {
          // A dropped chunk is lost footage, so it is a hard failure signal
          // (§7.3); the queue's own accounting is the authority (no second lock).
          seen_dropped = queue_stats.dropped;
          seen_rejected = queue_stats.rejected;
          recording_ok_.store(false);
          // Log the transition, not every drop: a sustained overload would
          // otherwise fill the log with one line per lost chunk, and on a device
          // with a small log partition that is its own outage.
          if (!logged_recording_failure_) {
            logged_recording_failure_ = true;
            HV_WARN("recording_ok=false: "
                    << seen_dropped << " chunk(s) dropped, " << seen_rejected
                    << " rejected");
          }
        }
      }

      // pop() drains a closed queue before failing, so this is the exit point.
      if (!popped && chunk_queue_ && chunk_queue_->closed()) break;
    }
  }

  /** Detect: one enabled label per interval, round robin, on the newest frame. */
  void detect_loop() {
    bool have_run = false;
    MonotonicMs last_run_ms = 0;
    while (!stopping_.load()) {
      std::shared_ptr<std::vector<uint8_t>> taken;
      Frame frame;
      {
        std::unique_lock<std::mutex> lock(mailbox_mutex_);
        mailbox_cv_.wait(lock, [this] {
          return mailbox_seq_ != detect_seen_seq_ || stopping_.load();
        });
        if (stopping_.load()) return;
        detect_seen_seq_ = mailbox_seq_;
        taken = mailbox_slot_;
        frame = mailbox_meta_;
        mailbox_ready_ = false;
      }
      if (!taken) continue;
      frame.data = taken->data();
      frame.size = taken->size();

      // Space the runs so no label can hold the inference resource: with three
      // labels each gets a turn every 3*interval_ms, well under the 5s
      // stale threshold (§9). Waited on a separate CV so the capture thread's
      // publish is never blocked by the pacing.
      if (have_run && config_.detect.interval_ms > 0) {
        const MonotonicMs due = last_run_ms + config_.detect.interval_ms;
        const MonotonicMs now = clock_.monotonic_ms();
        if (due > now) {
          std::unique_lock<std::mutex> lock(pace_mutex_);
          pace_cv_.wait_for(lock, std::chrono::milliseconds(due - now),
                            [this] { return stopping_.load(); });
          if (stopping_.load()) return;
        }
      }

      const size_t slot = pick_next_slot();
      if (slot < kEventTypeCount && detectors_[slot]) {
        const MonotonicMs run_at = clock_.monotonic_ms();
        std::vector<Detection> found;
        bool ran = false;
        {
          std::lock_guard<std::mutex> lock(detector_mutexes_[slot]);
          ran = detectors_[slot]->run(frame, found);
        }
        last_run_ms = run_at;
        have_run = true;
        if (ran) {
          // Freshness means "the label ran", not "it hit": a detector that runs
          // and finds nothing is healthy, so this is stamped on every
          // successful run.
          last_detection_ms_[slot].store(run_at);
          detection_runs_.fetch_add(1);
          detection_runs_by_label_[slot].fetch_add(1);
          for (auto& detection : found) {
            detection.ts_ms = run_at;
            detections_.fetch_add(1);
            if (hooks_.on_detection) hooks_.on_detection(detection, run_at);
          }
        }
      }

      // Give the buffer back for reuse. Safe: taking it cleared mailbox_slot_,
      // so publish cannot be holding a reference to it.
      {
        std::lock_guard<std::mutex> lock(mailbox_mutex_);
        if (mailbox_spare_.capacity() < taken->capacity()) {
          mailbox_spare_ = std::move(*taken);
        }
      }
    }
  }

  // --- frame mailbox -----------------------------------------------------

  void publish_frame(const Frame& frame) {
    if (!frame.valid()) return;
    {
      std::lock_guard<std::mutex> lock(mailbox_mutex_);
      if (mailbox_ready_) {
        // Overwrite always: the detect path only ever wants the newest frame,
        // so an unconsumed one is replaced rather than queued (design §5). The
        // replaced buffer is exclusively ours, so it is reclaimed for reuse.
        mailbox_overwrites_++;
        mailbox_spare_ = std::move(*mailbox_slot_);
        mailbox_slot_.reset();
      }

      // The single copy of pixel data in the pipeline. Frame::data belongs to
      // the camera and is invalidated by the next read(), so the detect thread
      // cannot share the camera's buffer; this copy is the one place the
      // scaffold is not board-ready -- phase A decides whether the MPP encoder
      // and the NPU can bind the same buffer with zero copy (design §8). It is
      // bounded at exactly one frame.
      auto buffer =
          std::make_shared<std::vector<uint8_t>>(std::move(mailbox_spare_));
      buffer->assign(frame.data, frame.data + frame.size);
      mailbox_meta_ = frame;
      mailbox_meta_.data = nullptr;  // metadata only; detect repoints to buffer
      mailbox_slot_ = std::move(buffer);
      mailbox_ready_ = true;
      mailbox_seq_++;
    }
    mailbox_cv_.notify_one();
  }

  // --- scheduling --------------------------------------------------------

  bool slot_enabled(size_t slot) const {
    // Person detection only runs while armed away (design §4.3); fire and smoke
    // are never disabled.
    if (slot == index_of(EventType::PersonIntrusion)) return person_enabled_.load();
    return true;
  }

  size_t pick_next_slot() {
    for (size_t i = 0; i < kEventTypeCount; ++i) {
      const size_t slot = next_slot_;
      next_slot_ = (next_slot_ + 1) % kEventTypeCount;
      if (slot_enabled(slot)) return slot;
    }
    return kEventTypeCount;  // unreachable while fire/smoke stay enabled
  }

  // --- Supervisor paths --------------------------------------------------

  Result restart_encoder() {
    std::lock_guard<std::mutex> lock(encoder_mutex_);
    if (!encoder_) return Result::error(Code::Internal, "encoder: not started");
    encoder_->close();
    const Result reopened = encoder_->open(encoder_config_);
    if (!reopened.ok()) {
      return Result::error(reopened.code(),
                           "encoder: restart failed: " + encoder_->last_error());
    }
    return Result::success();
  }

  Result restart_detector(EventType label) {
    const size_t slot = index_of(label);
    const DetectorConfig* detector_config = nullptr;
    for (const auto& candidate : detector_configs_) {
      if (candidate.label == label) {
        detector_config = &candidate;
        break;
      }
    }
    if (detector_config == nullptr) {
      return Result::error(Code::NotFound,
                           std::string("detector[") + to_wire(label) +
                               "]: no config");
    }
    std::lock_guard<std::mutex> lock(detector_mutexes_[slot]);
    auto& detector = detectors_[slot];
    if (!detector) {
      return Result::error(Code::Internal,
                           std::string("detector[") + to_wire(label) +
                               "]: not loaded");
    }
    detector->unload();
    const Result loaded = detector->load(*detector_config);
    if (!loaded.ok()) {
      return Result::error(loaded.code(),
                           std::string("detector[") + to_wire(label) +
                               "]: reload failed: " + detector->last_error());
    }
    return Result::success();
  }

  void request_keyframe() {
    std::lock_guard<std::mutex> lock(encoder_mutex_);
    if (encoder_) encoder_->request_keyframe();
  }

  std::vector<uint8_t> codec_config() const {
    std::lock_guard<std::mutex> lock(encoder_mutex_);
    if (!encoder_) return {};
    return encoder_->codec_config();
  }

  /**
   * Encodes a still from the newest frame, for the event snapshot. The pixels
   * are copied out under the mailbox lock and encoded outside it: on the board
   * the JPEG encoder is a hardware block, and holding the mailbox while it runs
   * would stall the detect thread on the one frame it needs.
   */
  bool capture_snapshot(std::vector<uint8_t>& jpeg) {
    std::vector<uint8_t> pixels;
    Frame frame;
    {
      std::lock_guard<std::mutex> lock(mailbox_mutex_);
      if (!mailbox_slot_ || mailbox_slot_->empty()) return false;
      pixels = *mailbox_slot_;
      frame = mailbox_meta_;
    }
    frame.data = pixels.data();
    frame.size = pixels.size();

    std::lock_guard<std::mutex> lock(encoder_mutex_);
    if (!encoder_) return false;
    return encoder_->encode_snapshot(frame, jpeg);
  }

  // --- reporting ---------------------------------------------------------

  QueueStatsSnapshot mailbox_snapshot() const {
    std::lock_guard<std::mutex> lock(mailbox_mutex_);
    QueueStatsSnapshot snapshot;
    snapshot.name = "frames";
    snapshot.pushed = mailbox_seq_;
    snapshot.dropped = mailbox_overwrites_;
    snapshot.depth = mailbox_ready_ ? 1 : 0;
    snapshot.capacity = 1;
    snapshot.popped = mailbox_seq_ - mailbox_overwrites_ - snapshot.depth;
    return snapshot;
  }

  PipelineStats snapshot() const {
    PipelineStats stats;
    stats.frames = frames_.load();
    stats.chunks = chunks_.load();
    stats.keyframes = keyframes_.load();
    stats.bytes_encoded = bytes_encoded_.load();
    stats.detection_runs = detection_runs_.load();
    stats.detections = detections_.load();
    for (size_t i = 0; i < kEventTypeCount; ++i) {
      stats.detection_runs_by_label[i] = detection_runs_by_label_[i].load();
      stats.last_detection_ms[i] = last_detection_ms_[i].load();
    }
    stats.last_frame_ms = last_frame_ms_.load();
    stats.last_encode_ms = last_encode_ms_.load();
    stats.recording_ok = recording_ok_.load();

    const QueueStatsSnapshot mailbox = mailbox_snapshot();
    // An overwritten frame is the detect path's equivalent of a dropped entry.
    stats.frames_dropped = mailbox.dropped;

    stats.queues.clear();
    QueueStatsSnapshot chunks;
    chunks.name = "chunks";
    if (chunk_queue_) {
      const BoundedQueue<EncodedChunk>::Stats queue_stats = chunk_queue_->stats();
      chunks.pushed = queue_stats.pushed;
      chunks.popped = queue_stats.popped;
      chunks.dropped = queue_stats.dropped;
      chunks.rejected = queue_stats.rejected;
      chunks.depth = queue_stats.depth;
      chunks.capacity = queue_stats.capacity;
    }
    stats.queues.push_back(chunks);
    stats.queues.push_back(mailbox);
    return stats;
  }

  Result unwind(Result failure) {
    stop();
    return failure;
  }

  // --- state -------------------------------------------------------------

  DeviceConfig config_;
  IClock& clock_;
  Hooks hooks_;

  std::unique_ptr<ICameraSource> camera_;
  std::unique_ptr<IEncoder> encoder_;
  std::array<std::unique_ptr<IDetector>, kEventTypeCount> detectors_;
  std::vector<DetectorConfig> detector_configs_;
  EncoderConfig encoder_config_;

  std::unique_ptr<BoundedQueue<EncodedChunk>> chunk_queue_;

  mutable std::mutex encoder_mutex_;
  std::array<std::mutex, kEventTypeCount> detector_mutexes_;

  // Mutable so the const snapshot()/codec_config() can read a consistent view.
  mutable std::mutex mailbox_mutex_;
  std::condition_variable mailbox_cv_;
  std::shared_ptr<std::vector<uint8_t>> mailbox_slot_;
  std::vector<uint8_t> mailbox_spare_;
  Frame mailbox_meta_;
  bool mailbox_ready_ = false;
  uint64_t mailbox_seq_ = 0;
  uint64_t mailbox_overwrites_ = 0;
  uint64_t detect_seen_seq_ = 0;

  std::mutex pace_mutex_;
  std::condition_variable pace_cv_;
  size_t next_slot_ = 0;

  std::atomic<uint64_t> frames_{0};
  std::atomic<uint64_t> chunks_{0};
  std::atomic<uint64_t> keyframes_{0};
  std::atomic<uint64_t> bytes_encoded_{0};
  std::atomic<uint64_t> detection_runs_{0};
  std::atomic<uint64_t> detections_{0};
  std::array<std::atomic<uint64_t>, kEventTypeCount> detection_runs_by_label_;
  std::atomic<MonotonicMs> last_frame_ms_{-1};
  std::atomic<MonotonicMs> last_encode_ms_{-1};
  std::array<std::atomic<MonotonicMs>, kEventTypeCount> last_detection_ms_;
  std::atomic<bool> recording_ok_{true};
  /** Only the record thread touches this: one warning per recording failure. */
  bool logged_recording_failure_ = false;
  std::atomic<bool> person_enabled_{true};
  std::atomic<bool> stopping_{false};

  std::thread capture_thread_;
  std::thread record_thread_;
  std::thread detect_thread_;
  bool started_ = false;
};

Pipeline::Pipeline(const DeviceConfig& config, IClock& clock, Hooks hooks)
    : impl_(std::make_unique<Impl>(config, clock, std::move(hooks))) {}

Pipeline::~Pipeline() = default;

Result Pipeline::start() { return impl_->start(); }

void Pipeline::stop() { impl_->stop(); }

PipelineStats Pipeline::stats() const { return impl_->snapshot(); }

std::vector<uint8_t> Pipeline::codec_config() const {
  return impl_->codec_config();
}

bool Pipeline::capture_snapshot(std::vector<uint8_t>& jpeg) {
  return impl_->capture_snapshot(jpeg);
}

void Pipeline::set_person_detection_enabled(bool enabled) {
  impl_->person_enabled_.store(enabled);
}

Result Pipeline::restart_encoder() { return impl_->restart_encoder(); }

Result Pipeline::restart_detector(EventType label) {
  return impl_->restart_detector(label);
}

void Pipeline::request_keyframe() { impl_->request_keyframe(); }

}  // namespace hv
