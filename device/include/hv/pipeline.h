#pragma once

// The capture/encode/detect pipeline: three threads, bounded queues, and the
// per-label detection scheduler.
//
// Design §4.2 (never block the camera, bounded queues), §4.4 (fire and smoke
// run regardless of motion; no label may starve another), §9 (every label's
// completion gap must stay under the hub's stale threshold).

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "hv/clock.h"
#include "hv/config.h"
#include "hv/status.h"
#include "hv/types.h"

namespace hv {

struct QueueStatsSnapshot {
  std::string name;
  uint64_t pushed = 0;
  uint64_t popped = 0;
  uint64_t dropped = 0;
  uint64_t rejected = 0;
  size_t depth = 0;
  size_t capacity = 0;
};

struct PipelineStats {
  uint64_t frames = 0;
  uint64_t frames_dropped = 0;
  uint64_t chunks = 0;
  uint64_t keyframes = 0;
  uint64_t bytes_encoded = 0;
  uint64_t detection_runs = 0;
  uint64_t detections = 0;
  std::array<uint64_t, kEventTypeCount> detection_runs_by_label{};
  /** -1 means "never", which the hub renders as null. */
  MonotonicMs last_frame_ms = -1;
  MonotonicMs last_encode_ms = -1;
  std::array<MonotonicMs, kEventTypeCount> last_detection_ms{{-1, -1, -1}};
  /** False once the record path had to drop or reject encoded data. */
  bool recording_ok = true;
  std::vector<QueueStatsSnapshot> queues;
};

class Pipeline {
 public:
  struct Hooks {
    /** Called from the record thread for every encoded chunk. */
    std::function<void(const EncodedChunk&, MonotonicMs)> on_chunk;
    /** Called from the detection thread for every accepted detection. */
    std::function<void(const Detection&, MonotonicMs)> on_detection;
  };

  Pipeline(const DeviceConfig& config, IClock& clock, Hooks hooks);
  ~Pipeline();

  Result start();
  void stop();

  PipelineStats stats() const;

  /** Stage rebuild paths used by the Supervisor. */
  Result restart_encoder();
  void request_keyframe();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace hv
