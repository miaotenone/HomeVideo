#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "harness.h"
#include "hv/clock.h"
#include "hv/config.h"
#include "hv/pipeline.h"
#include "hv/platform.h"

using namespace hv;

namespace {

// Small and non-realtime so the synthetic camera runs without sleeping; a short
// detection interval so the round robin visits all three labels quickly.
DeviceConfig fast_config() {
  DeviceConfig config = default_config();
  config.capture.width = 320;
  config.capture.height = 180;
  config.capture.fps = 30;
  config.capture.realtime = false;
  config.encode.gop_ms = 200;      // a keyframe within the first few frames
  config.detect.interval_ms = 20;  // one detector run per interval
  return config;
}

struct HookLog {
  std::mutex mutex;
  std::vector<EventType> detections;
  uint64_t chunks = 0;
  uint64_t chunk_bytes = 0;
  MonotonicMs chunk_ms = -1;
  int chunk_delay_ms = 0;  // set before start() for the slow-consumer case
};

Pipeline::Hooks hooks_for(const std::shared_ptr<HookLog>& log) {
  Pipeline::Hooks hooks;
  hooks.on_chunk = [log](const EncodedChunk& chunk, MonotonicMs at) {
    if (log->chunk_delay_ms > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(log->chunk_delay_ms));
    }
    std::lock_guard<std::mutex> lock(log->mutex);
    log->chunks++;
    log->chunk_bytes += chunk.data.size();
    log->chunk_ms = at;
  };
  hooks.on_detection = [log](const Detection& detection, MonotonicMs) {
    std::lock_guard<std::mutex> lock(log->mutex);
    log->detections.push_back(detection.label);
  };
  return hooks;
}

// Bounded poll: never an unbounded wait.
bool wait_until(const std::function<bool()>& ready, int budget_ms = 2000,
                int step_ms = 10) {
  for (int waited = 0; waited < budget_ms; waited += step_ms) {
    if (ready()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(step_ms));
  }
  return ready();
}

const QueueStatsSnapshot* find_queue(const PipelineStats& stats, const char* name) {
  for (const auto& queue : stats.queues) {
    if (queue.name == name) return &queue;
  }
  return nullptr;
}

bool log_saw_label(const std::shared_ptr<HookLog>& log, EventType label) {
  std::lock_guard<std::mutex> lock(log->mutex);
  for (const EventType seen : log->detections) {
    if (seen == label) return true;
  }
  return false;
}

SimulatedDetection always_detects() {
  SimulatedDetection script;
  script.at_run = 1;
  script.runs = 1000000;  // effectively "every run"
  script.confidence = 0.9f;
  return script;
}

}  // namespace

HV_TEST(pipeline_start_encodes_and_reports_queues) {
  const DeviceConfig config = fast_config();
  SystemClock clock;
  auto log = std::make_shared<HookLog>();
  Pipeline pipeline(config, clock, hooks_for(log));

  const Result started = pipeline.start();
  HV_CHECK(started.ok());
  HV_CHECK_FALSE(pipeline.codec_config().empty());

  const bool produced = wait_until([&] {
    const PipelineStats stats = pipeline.stats();
    return stats.frames > 0 && stats.chunks > 0 && stats.keyframes > 0;
  });
  HV_CHECK(produced);

  const PipelineStats stats = pipeline.stats();
  HV_CHECK(stats.frames > 0);
  HV_CHECK(stats.chunks > 0);
  HV_CHECK(stats.keyframes > 0);
  HV_CHECK(stats.last_frame_ms >= 0);
  HV_CHECK(stats.last_encode_ms >= 0);
  HV_CHECK_TRUE(stats.recording_ok);

  const QueueStatsSnapshot* chunks = find_queue(stats, "chunks");
  const QueueStatsSnapshot* frames = find_queue(stats, "frames");
  HV_CHECK(chunks != nullptr);
  HV_CHECK(frames != nullptr);
  if (chunks != nullptr) {
    HV_CHECK_EQ(chunks->capacity, size_t{8});
    HV_CHECK(chunks->depth <= chunks->capacity);
  }
  if (frames != nullptr) {
    HV_CHECK_EQ(frames->capacity, size_t{1});
    HV_CHECK(frames->depth <= frames->capacity);
  }

  pipeline.stop();
}

HV_TEST(pipeline_snapshot_is_a_jpeg_from_the_newest_frame) {
  const DeviceConfig config = fast_config();
  SystemClock clock;
  auto log = std::make_shared<HookLog>();
  Pipeline pipeline(config, clock, hooks_for(log));

  // Before a frame has been published there is nothing to encode a still from.
  std::vector<uint8_t> jpeg;
  HV_CHECK_FALSE(pipeline.capture_snapshot(jpeg));
  HV_CHECK(jpeg.empty());

  HV_CHECK(pipeline.start().ok());
  const bool produced = wait_until([&] { return pipeline.stats().frames > 0; });
  HV_CHECK(produced);
  HV_CHECK_TRUE(pipeline.capture_snapshot(jpeg));
  HV_CHECK(jpeg.size() > 128);
  HV_CHECK_EQ(jpeg[0], uint8_t{0xff});
  HV_CHECK_EQ(jpeg[1], uint8_t{0xd8});

  pipeline.stop();
}

HV_TEST(pipeline_reports_scripted_detections_through_the_hook) {
  DeviceConfig config = fast_config();
  // default_config() lays the detectors out as person, fire, smoke.
  HV_CHECK_EQ(config.detect.detectors[1].label, EventType::FireSuspected);
  config.detect.detectors[1].simulation = {always_detects()};

  SystemClock clock;
  auto log = std::make_shared<HookLog>();
  Pipeline pipeline(config, clock, hooks_for(log));
  HV_CHECK(pipeline.start().ok());

  const bool fired = wait_until([&] {
    return pipeline.stats().detection_runs_by_label[1] > 0 &&
           log_saw_label(log, EventType::FireSuspected);
  });
  HV_CHECK(fired);

  const PipelineStats stats = pipeline.stats();
  HV_CHECK(stats.detection_runs_by_label[1] > 0);
  HV_CHECK(stats.detections > 0);
  HV_CHECK_TRUE(log_saw_label(log, EventType::FireSuspected));

  pipeline.stop();
}

HV_TEST(pipeline_stamps_freshness_even_without_a_hit) {
  const DeviceConfig config = fast_config();  // no simulation scripted anywhere
  SystemClock clock;
  auto log = std::make_shared<HookLog>();
  Pipeline pipeline(config, clock, hooks_for(log));
  HV_CHECK(pipeline.start().ok());

  // Freshness is "the label ran", not "the label hit".
  const bool all_ran = wait_until([&] {
    const PipelineStats stats = pipeline.stats();
    return stats.last_detection_ms[0] >= 0 && stats.last_detection_ms[1] >= 0 &&
           stats.last_detection_ms[2] >= 0;
  });
  HV_CHECK(all_ran);

  const PipelineStats stats = pipeline.stats();
  HV_CHECK(stats.last_detection_ms[0] >= 0);
  HV_CHECK(stats.last_detection_ms[1] >= 0);
  HV_CHECK(stats.last_detection_ms[2] >= 0);
  HV_CHECK(stats.detections == 0);  // nothing was scripted to fire

  pipeline.stop();
}

HV_TEST(pipeline_person_detection_toggles_without_starving_fire_and_smoke) {
  const DeviceConfig config = fast_config();
  SystemClock clock;
  auto log = std::make_shared<HookLog>();
  Pipeline pipeline(config, clock, hooks_for(log));
  HV_CHECK(pipeline.start().ok());

  const bool person_ran = wait_until([&] {
    return pipeline.stats().last_detection_ms[0] >= 0;
  });
  HV_CHECK(person_ran);

  const PipelineStats before = pipeline.stats();
  const uint64_t person_before = before.detection_runs_by_label[0];
  const uint64_t fire_before = before.detection_runs_by_label[1];
  const uint64_t smoke_before = before.detection_runs_by_label[2];

  pipeline.set_person_detection_enabled(false);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  const PipelineStats disabled = pipeline.stats();
  // Allow one in-flight run to land rather than demanding an exact freeze.
  HV_CHECK(disabled.detection_runs_by_label[0] - person_before <= 1);
  HV_CHECK(disabled.detection_runs_by_label[1] > fire_before ||
           disabled.detection_runs_by_label[2] > smoke_before);

  const uint64_t person_disabled = disabled.detection_runs_by_label[0];
  pipeline.set_person_detection_enabled(true);
  const bool resumed = wait_until([&] {
    return pipeline.stats().detection_runs_by_label[0] > person_disabled;
  });
  HV_CHECK(resumed);

  pipeline.stop();
}

HV_TEST(pipeline_restart_encoder_keeps_chunks_flowing) {
  const DeviceConfig config = fast_config();
  SystemClock clock;
  auto log = std::make_shared<HookLog>();
  Pipeline pipeline(config, clock, hooks_for(log));
  HV_CHECK(pipeline.start().ok());

  HV_CHECK(wait_until([&] { return pipeline.stats().chunks > 0; }));
  const uint64_t before = pipeline.stats().chunks;

  HV_CHECK(pipeline.restart_encoder().ok());
  HV_CHECK_FALSE(pipeline.codec_config().empty());
  const bool flowing = wait_until([&] { return pipeline.stats().chunks > before; });
  HV_CHECK(flowing);

  pipeline.stop();
}

HV_TEST(pipeline_request_keyframe_is_safe_and_chunks_keep_flowing) {
  const DeviceConfig config = fast_config();
  SystemClock clock;
  auto log = std::make_shared<HookLog>();
  Pipeline pipeline(config, clock, hooks_for(log));
  HV_CHECK(pipeline.start().ok());

  HV_CHECK(wait_until([&] { return pipeline.stats().chunks > 0; }));
  const uint64_t before = pipeline.stats().chunks;

  pipeline.request_keyframe();  // must not throw
  const bool flowing = wait_until([&] { return pipeline.stats().chunks > before; });
  HV_CHECK(flowing);

  pipeline.stop();
}

HV_TEST(pipeline_stop_joins_threads_and_is_idempotent) {
  const DeviceConfig config = fast_config();
  SystemClock clock;
  auto log = std::make_shared<HookLog>();
  Pipeline pipeline(config, clock, hooks_for(log));
  HV_CHECK(pipeline.start().ok());
  HV_CHECK(wait_until([&] { return pipeline.stats().frames > 0; }));

  pipeline.stop();
  const PipelineStats stats = pipeline.stats();
  const QueueStatsSnapshot* chunks = find_queue(stats, "chunks");
  const QueueStatsSnapshot* frames = find_queue(stats, "frames");
  HV_CHECK(chunks != nullptr);
  HV_CHECK(frames != nullptr);
  if (chunks != nullptr) HV_CHECK_EQ(chunks->depth, size_t{0});
  if (frames != nullptr) HV_CHECK_EQ(frames->depth, size_t{0});

  pipeline.stop();  // harmless a second time
  HV_CHECK_TRUE(find_queue(pipeline.stats(), "chunks") != nullptr);
}

HV_TEST(pipeline_restarts_after_stop) {
  const DeviceConfig config = fast_config();
  SystemClock clock;
  auto log = std::make_shared<HookLog>();
  Pipeline pipeline(config, clock, hooks_for(log));

  HV_CHECK(pipeline.start().ok());
  HV_CHECK(wait_until([&] { return pipeline.stats().frames > 0; }));
  pipeline.stop();
  const uint64_t frames_before = pipeline.stats().frames;

  // A second start() must also rebuild the queues, which close() leaves closed.
  HV_CHECK(pipeline.start().ok());
  HV_CHECK(wait_until([&] { return pipeline.stats().frames > frames_before; }));
  HV_CHECK_TRUE(pipeline.stats().recording_ok);
  pipeline.stop();
}

HV_TEST(pipeline_start_fails_and_stays_safe_when_camera_cannot_open) {
  DeviceConfig config = fast_config();
  config.capture.width = 0;  // the desktop camera refuses invalid options

  SystemClock clock;
  auto log = std::make_shared<HookLog>();
  Pipeline pipeline(config, clock, hooks_for(log));

  const Result started = pipeline.start();
  HV_CHECK_FALSE(started.ok());
  HV_CHECK(started.message().find("camera") != std::string::npos);
  HV_CHECK_TRUE(pipeline.codec_config().empty());

  pipeline.stop();  // safe after a partial start
  pipeline.stop();
}

HV_TEST(pipeline_marks_recording_failed_when_chunks_are_dropped) {
  const DeviceConfig config = fast_config();
  SystemClock clock;
  auto log = std::make_shared<HookLog>();
  log->chunk_delay_ms = 50;  // slow consumer against a producer that never sleeps
  Pipeline pipeline(config, clock, hooks_for(log));
  HV_CHECK(pipeline.start().ok());

  const bool failed = wait_until([&] { return !pipeline.stats().recording_ok; });
  HV_CHECK(failed);

  const PipelineStats stats = pipeline.stats();
  const QueueStatsSnapshot* chunks = find_queue(stats, "chunks");
  HV_CHECK_FALSE(stats.recording_ok);
  HV_CHECK(chunks != nullptr);
  if (chunks != nullptr) {
    HV_CHECK(chunks->dropped > 0);
    HV_CHECK(chunks->depth <= chunks->capacity);  // bounded, not growing
  }

  pipeline.stop();
}
