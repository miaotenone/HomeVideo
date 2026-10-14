#pragma once

// Stage supervision (design §7.3): notice a stalled stage, ask for it to be
// rebuilt, and give up after a bounded number of attempts instead of restarting
// forever.
//
// Rebuilding is expressed as a callback rather than a Pipeline reference so the
// policy is fully testable without hardware or threads; the App wires the
// callback to Pipeline::restart_encoder / restart_detector.
//
// Only liveness is policed here. "A detector has never run" is not a fault when
// that detector is disabled (person detection is off unless armed away), which
// is why callers pass the enabled set.

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "hv/health.h"
#include "hv/pipeline.h"
#include "hv/status.h"
#include "hv/types.h"

namespace hv {

struct RecoveryAction {
  MonotonicMs at_ms = 0;
  std::string stage;
  std::string reason;
};

class Supervisor {
 public:
  /** Rebuilds one stage. Returns non-Ok when the rebuild itself failed. */
  using Repair = std::function<Result(const std::string& stage)>;

  Supervisor(HealthThresholds thresholds, int max_restarts);

  /**
   * Inspects liveness and asks for each stalled, enabled stage to be rebuilt.
   * A stage is only asked about once per stall: after a rebuild it gets the
   * staleness window again before it is considered stalled once more.
   */
  std::vector<RecoveryAction> check(MonotonicMs now, const PipelineStats& stats,
                                     const std::array<bool, kEventTypeCount>& enabled,
                                     const Repair& repair);

  const std::vector<RecoveryAction>& history() const { return history_; }
  uint64_t restart_count() const { return history_.size(); }
  bool budget_exhausted() const;

  /** Stage names used in RecoveryAction and passed to the Repair callback. */
  static const char* encoder_stage();
  static std::string detector_stage(EventType label);

 private:
  /** True when `last_ok` is stale: never produced anything and past the grace. */
  bool stalled(MonotonicMs now, MonotonicMs last_ok, MonotonicMs threshold) const;
  void note_restart(MonotonicMs now, std::string stage, std::string reason);

  HealthThresholds thresholds_;
  int max_restarts_;
  /** Index 0 is the encoder, then one slot per label (see index_of). */
  std::array<MonotonicMs, kEventTypeCount + 1> last_restart_ms_{};
  std::vector<RecoveryAction> history_;
};

}  // namespace hv
