#pragma once

// Stage supervision (design §7.3): report and recover a stalled stage, in the
// order module rebuild -> process restart -> watchdog. This batch implements the
// first step plus the restart budget; the process-level restart is the process
// manager's job (see device/README.md).

#include <cstdint>
#include <string>
#include <vector>

#include "hv/health.h"
#include "hv/pipeline.h"
#include "hv/types.h"

namespace hv {

struct RecoveryAction {
  MonotonicMs at_ms = 0;
  std::string stage;
  std::string reason;
};

class Supervisor {
 public:
  Supervisor(HealthThresholds thresholds, int max_restarts);

  /** Inspects liveness and rebuilds a stalled stage, within the restart budget. */
  std::vector<RecoveryAction> check(MonotonicMs now, const PipelineStats& stats,
                                    Pipeline& pipeline);

  const std::vector<RecoveryAction>& history() const { return history_; }
  uint64_t restart_count() const { return history_.size(); }
  bool budget_exhausted() const;

 private:
  bool stalled(MonotonicMs now, MonotonicMs last_ok, MonotonicMs threshold) const;

  HealthThresholds thresholds_;
  int max_restarts_;
  MonotonicMs encode_restart_at_ = -1;
  MonotonicMs detect_restart_at_ = -1;
  std::vector<RecoveryAction> history_;
};

}  // namespace hv
