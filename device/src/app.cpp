#include "hv/app.h"

// Top-level wiring. The one subtlety in this file is the locking contract:
// EventManager has no locks of its own, and the pipeline calls its hooks from
// the record and detection threads. Every EventManager access therefore happens
// under `mutex_`, and the pipeline never calls a hook while holding its mailbox
// or encoder lock. The order is always App-mutex -> (mailbox, encoder) and never
// the reverse, so `request_keyframe` and `capture_snapshot` (both called from a
// hook) cannot deadlock against the capture thread.

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "hv/clock.h"
#include "hv/config.h"
#include "hv/events.h"
#include "hv/health.h"
#include "hv/hub_client.h"
#include "hv/log.h"
#include "hv/pipeline.h"
#include "hv/spool.h"
#include "hv/supervisor.h"
#include "hv/uploader.h"

namespace hv {

namespace {

// A signal handler may only touch lock-free state, so the request is a single
// atomic flag that the main loop notices on its next iteration.
std::atomic<bool> g_stop_requested{false};

void request_stop(int) { g_stop_requested.store(true); }

// Fixed label order matching index_of(); kept explicit so a future label cannot
// silently shift a slot.
const std::array<EventType, kEventTypeCount>& labels() {
  static const std::array<EventType, kEventTypeCount> kLabels{
      EventType::PersonIntrusion, EventType::FireSuspected,
      EventType::SmokeSuspected};
  return kLabels;
}

LogLevel parse_log_level(const std::string& name) {
  if (name == "debug") return LogLevel::Debug;
  if (name == "warn") return LogLevel::Warn;
  if (name == "error") return LogLevel::Error;
  return LogLevel::Info;
}

// Bounded upload drain used on shutdown. A dead hub must not hang the process,
// and --once depends on it finishing with a fully uploaded event.
constexpr int kMaxDrainPasses = 50;
constexpr size_t kDrainBatch = 16;

}  // namespace

struct App::Impl {
  Impl(DeviceConfig config, std::unique_ptr<IClock> clock)
      : config_(std::move(config)),
        clock_(std::move(clock)),
        time_(*clock_),
        boot_id_(new_boot_id()),
        spool_(config_.spool_dir,
               QuotaConfig{config_.quota.rolling_bytes,
                           config_.quota.pending_bytes,
                           config_.quota.uploaded_bytes,
                           config_.quota.metadata_bytes}),
        client_(config_.hub_url, config_.device_id,
                config_.upload.http_timeout_ms),
        health_(config_.device_id, boot_id_),
        arm_(client_, *clock_, config_.spool_dir / "state"),
        events_(EventPolicy{config_.event.pre_roll_ms, config_.event.post_roll_ms,
                            config_.event.merge_window_ms,
                            config_.event.segment_ms, config_.event.segment_bytes,
                            config_.detect.confirm_frames,
                            config_.detect.confirm_hold_ms},
                config_.device_id, boot_id_, time_,
                config_.event.ring_buffer_bytes),
        uploader_(spool_, client_, *clock_,
                  UploaderLimits{BackoffPolicy{500, config_.upload.max_backoff_ms,
                                               config_.upload.max_attempts, 0.25},
                                 config_.upload.rate_bytes_per_s, 16}),
        pipeline_(config_, *clock_,
                  Pipeline::Hooks{
                      [this](const EncodedChunk& chunk, MonotonicMs now) {
                        on_chunk(chunk, now);
                      },
                      [this](const Detection& detection, MonotonicMs now) {
                        on_detection(detection, now);
                      }}),
        supervisor_(HealthThresholds{config_.health.video_stale_ms,
                                     config_.health.recording_stale_ms},
                    config_.health.max_stage_restarts) {
    // The handlers need the other members to exist, which is why the Pipeline
    // captures `this` and only starts its threads in run().
    events_.set_handlers(
        [this](EventRecord& record) { handle_open(record); },
        [this](EventRecord& record, FinishedSegment& segment) {
          handle_segment(record, segment);
        },
        [this](EventRecord& record) { handle_close(record); },
        [this] { pipeline_.request_keyframe(); });

    // Every event carries the model that produced it, not a global default.
    for (const DetectorSpec& detector : config_.detect.detectors) {
      events_.set_model_version(detector.label, detector.model_version);
    }
  }

  // --- pipeline hooks: called from the record and detection threads ---------

  void on_chunk(const EncodedChunk& chunk, MonotonicMs now) {
    std::lock_guard<std::mutex> lock(mutex_);
    events_.on_chunk(chunk, now);
  }

  void on_detection(const Detection& detection, MonotonicMs now) {
    std::lock_guard<std::mutex> lock(mutex_);
    events_.on_detection(detection, now);
  }

  // --- EventManager handlers: already under `mutex_` -----------------------

  void handle_open(EventRecord& record) {
    Manifest manifest;
    manifest.manifest_version = 1;
    manifest.event_id = record.event_id;
    manifest.device_id = config_.device_id;
    manifest.event_type = record.type_wire();
    manifest.started_at = record.started_at_unix;
    manifest.time_trusted = record.time_trusted;
    manifest.device_boot_id = boot_id_;
    manifest.device_started_ms = record.start_ms;
    manifest.model_version = record.model_version;
    manifest.config_version = record.config_version;
    manifest.arm_mode = to_wire(record.arm_mode);
    manifest.detections = record.detections;
    manifest.codec.codec = config_.encode.codec;
    manifest.codec.width = config_.capture.width;
    manifest.codec.height = config_.capture.height;
    manifest.codec.fps = config_.capture.fps;
    manifest.codec.bitrate_bps = config_.encode.bitrate_bps;
    manifest.codec.gop_ms = config_.encode.gop_ms;
    manifest.segments = {};
    manifest.state = "active";

    const Result created = spool_.create_event(manifest);
    if (!created.ok()) {
      HV_ERROR("cannot create event " << record.event_id << ": "
                                      << created.describe());
    }

    // Metadata first (above), then the thumbnail. A backend with no JPEG path
    // (or a frame that has not arrived yet) simply has no snapshot; that must
    // not stop the event from being recorded and uploaded.
    std::vector<uint8_t> jpeg;
    if (pipeline_.capture_snapshot(jpeg)) {
      const Result written =
          spool_.write_snapshot(record.event_id, jpeg, "image/jpeg");
      if (!written.ok()) {
        HV_WARN("snapshot write failed for " << record.event_id << ": "
                                             << written.describe());
      }
    }
  }

  void handle_segment(EventRecord& record, FinishedSegment& segment) {
    const Result appended = spool_.append_segment(record.event_id, segment);
    if (!appended.ok()) {
      HV_WARN("segment append failed for " << record.event_id << ": "
                                           << appended.describe());
    }
  }

  void handle_close(EventRecord& record) {
    const Result closed = spool_.mark_closed(record.event_id, record.end_ms,
                                             record.duration_ms(),
                                             record.time_trusted);
    if (!closed.ok()) {
      HV_WARN("mark_closed failed for " << record.event_id << ": "
                                        << closed.describe());
    }
  }

  // --- lifecycle -----------------------------------------------------------

  int run(const RunOptions& options) {
    set_log_level(parse_log_level(config_.log_level));
    if (options.verbose) set_log_level(LogLevel::Debug);

    const Result prepared = spool_.prepare();
    if (!prepared.ok()) {
      HV_ERROR("spool prepare failed: " << prepared.describe());
      return 1;
    }
    // The crash report from a previous process run: anything left active never
    // got to close, so it is finalised as interrupted and still uploadable.
    for (const std::string& id : spool_.mark_interrupted_events()) {
      HV_WARN("event " << id << " interrupted by a previous shutdown");
    }

    const Result restored = arm_.load_persisted();
    if (!restored.ok()) {
      HV_WARN("arm state load failed: " << restored.describe());
    }
    events_.set_arm_mode(arm_.applied_mode(), arm_.applied_version());
    pipeline_.set_person_detection_enabled(arm_.should_run_person_detection());

    const Result started = pipeline_.start();
    if (!started.ok()) {
      HV_ERROR("pipeline start failed: " << started.describe());
      return 1;
    }
    {
      // Segments need the encoder's SPS/PPS to be self-describing; the record
      // thread is already running, so this shares the EventManager lock.
      std::lock_guard<std::mutex> lock(mutex_);
      events_.set_codec_config(pipeline_.codec_config());
    }

    g_stop_requested.store(false);
    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);

    const HealthThresholds thresholds{config_.health.video_stale_ms,
                                      config_.health.recording_stale_ms};
    const Supervisor::Repair repair = [this](const std::string& stage) {
      if (stage == "encoder") return pipeline_.restart_encoder();
      const std::string prefix = "detector:";
      if (stage.rfind(prefix, 0) == 0) {
        const auto label = event_type_from_wire(stage.substr(prefix.size()));
        if (label.has_value()) return pipeline_.restart_detector(*label);
      }
      return Result::error(Code::NotFound, "unknown stage: " + stage);
    };

    const MonotonicMs loop_start = clock_->monotonic_ms();
    MonotonicMs last_heartbeat_ms = -1;

    while (!g_stop_requested.load()) {
      const MonotonicMs now = clock_->monotonic_ms();
      if (options.once && now - loop_start >= options.once_budget_ms) break;

      // Arm: poll periodically, and re-publish the applied mode regardless of
      // whether the poll succeeded (a failed poll leaves it unchanged).
      if (arm_.last_poll_ms() < 0 ||
          now - arm_.last_poll_ms() >= config_.upload.arm_poll_ms) {
        const Result polled = arm_.poll_and_apply();
        if (!polled.ok()) {
          HV_DEBUG("arm poll failed: " << polled.describe());
        }
        {
          std::lock_guard<std::mutex> lock(mutex_);
          events_.set_arm_mode(arm_.applied_mode(), arm_.applied_version());
        }
        pipeline_.set_person_detection_enabled(
            arm_.should_run_person_detection());
      }

      // Heartbeat: the response Date header is the only thing that ever makes
      // the wall clock trustworthy (see TimeSource).
      if (last_heartbeat_ms < 0 ||
          now - last_heartbeat_ms >= config_.upload.heartbeat_ms) {
        last_heartbeat_ms = now;
        const DeviceReport report =
            health_.build(now, thresholds, arm_.applied_mode(),
                          arm_.applied_version(), time_.trusted());
        const HeartbeatResult beat = client_.heartbeat(report);
        if (beat.server_unix.has_value()) {
          time_.observe_server_unix(*beat.server_unix);
        }
      }

      {
        std::lock_guard<std::mutex> lock(mutex_);
        events_.tick(now);
        for (const EventRecord& record : events_.take_finished()) {
          HV_INFO("event " << record.event_id
                           << " finished duration_ms=" << record.duration_ms());
        }
      }

      const PipelineStats stats = pipeline_.stats();
      // Person detection is off unless armed away; fire and smoke always run.
      const std::array<bool, kEventTypeCount> enabled{
          arm_.should_run_person_detection(), true, true};
      for (const RecoveryAction& action :
           supervisor_.check(now, stats, enabled, repair)) {
        HV_WARN("supervisor rebuilt " << action.stage << ": " << action.reason);
      }

      health_.note_frame(stats.last_frame_ms);
      health_.note_encode(stats.last_encode_ms);
      for (size_t i = 0; i < kEventTypeCount; ++i) {
        // Only a label that has actually run may report a freshness stamp.
        if (stats.detection_runs_by_label[i] > 0) {
          health_.note_detection(labels()[i], stats.last_detection_ms[i]);
        }
      }
      health_.note_recording_ok(stats.recording_ok, now);
      try {
        health_.note_storage_free(
            static_cast<uint64_t>(std::filesystem::space(config_.spool_dir)
                                      .available));
      } catch (const std::exception& error) {
        // An unreadable filesystem must not kill the loop; the last known value
        // stays and is simply not refreshed.
        HV_DEBUG("storage free query failed: " << error.what());
      }

      uploader_.run_for(50);
      // A real sleep, deliberately not derived from the injected clock: tests
      // drive the loop with a SystemClock and this is what paces it.
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Shutdown, shared by --once and SIGINT/SIGTERM. Stopping the pipeline
    // first means no new chunk or detection can race the final close below.
    pipeline_.stop();
    {
      // force_close_all, never mark_interrupted: recording is complete up to
      // now, so these events are Closed rather than left for the next boot.
      std::lock_guard<std::mutex> lock(mutex_);
      events_.force_close_all(clock_->monotonic_ms());
      for (const EventRecord& record : events_.take_finished()) {
        HV_INFO("event " << record.event_id
                         << " closed on shutdown duration_ms="
                         << record.duration_ms());
      }
    }
    drain_uploads();

    const Result persisted = arm_.persist();
    if (!persisted.ok()) {
      HV_WARN("arm state persist failed: " << persisted.describe());
    }
    return 0;
  }

 private:
  // Works the pending queue until it is empty or the pass budget is spent.
  void drain_uploads() {
    for (int pass = 0; pass < kMaxDrainPasses; ++pass) {
      if (spool_.pending(1).empty()) return;
      const size_t attempted = uploader_.run_items(kDrainBatch);
      if (attempted == 0) {
        // Every remaining item is backing off or rate-limited; give the real
        // clock a moment instead of spinning.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    }
  }

  DeviceConfig config_;
  std::unique_ptr<IClock> clock_;
  TimeSource time_;
  std::string boot_id_;
  Spool spool_;
  HubClient client_;
  HealthMonitor health_;
  ArmController arm_;
  EventManager events_;
  Uploader uploader_;
  Pipeline pipeline_;
  Supervisor supervisor_;
  /** Serialises every EventManager access (see the file header). */
  std::mutex mutex_;
};

App::App(DeviceConfig config, std::unique_ptr<IClock> clock)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(clock))) {}

App::~App() = default;

int App::run(const RunOptions& options) { return impl_->run(options); }

}  // namespace hv
