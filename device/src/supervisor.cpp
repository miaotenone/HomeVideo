#include "hv/supervisor.h"

#include <utility>

// Stage supervision policy (design §7.3). Three rules carry the weight here and
// each exists to break a specific runaway loop:
//   - a disabled stage is invisible: person detection is off unless the device
//     is armed away, so "never ran" would otherwise be a permanent fault that is
//     rebuilt forever (see health.h on why detection trouble is staleness only).
//   - a rebuilt stage gets the staleness window again (cooldown), so one stage
//     that refuses to recover is not rebuilt on every loop iteration.
//   - a failed rebuild still spends budget, so a permanently broken stage cannot
//     be retried until the end of time.
namespace hv {

namespace {

// The label order that lines up with index_of() and the last_restart_ms_ slots.
// Kept explicit so a future label cannot silently shift a slot.
constexpr std::array<EventType, kEventTypeCount> kLabels{{
    EventType::PersonIntrusion,
    EventType::FireSuspected,
    EventType::SmokeSuspected,
}};

// Gap since the last successful output. The -1 "never" sentinel has no gap of
// its own, so the whole uptime is the honest answer.
MonotonicMs stall_age_ms(MonotonicMs now, MonotonicMs last_ok) {
  return last_ok < 0 ? now : now - last_ok;
}

}  // namespace

Supervisor::Supervisor(HealthThresholds thresholds, int max_restarts)
    : thresholds_(thresholds), max_restarts_(max_restarts) {
  // 0 reads as "never rebuilt". That is sound because the grace period already
  // suppresses every check before now >= threshold, so a fresh Supervisor can
  // never look like it is still inside a cooldown it never entered.
  last_restart_ms_.fill(0);
}

bool Supervisor::stalled(MonotonicMs now, MonotonicMs last_ok,
                         MonotonicMs threshold) const {
  // The grace period is measured from process start (now is monotonic-since-
  // start). Without it, a device whose stages need a moment to come up would
  // repair them before they had any chance to produce output.
  if (now < threshold) return false;
  // Never produced anything, or the last output is older than the window.
  return last_ok < 0 || now - last_ok > threshold;
}

std::vector<RecoveryAction> Supervisor::check(
    MonotonicMs now, const PipelineStats& stats,
    const std::array<bool, kEventTypeCount>& enabled, const Repair& repair) {
  std::vector<RecoveryAction> actions;
  // Bounded repair budget: once spent, do nothing at all. Otherwise a stage that
  // can never be fixed keeps the whole repair machinery busy forever.
  if (budget_exhausted()) return actions;

  const MonotonicMs threshold = thresholds_.video_stale_ms;

  const auto consider = [&](size_t slot, const std::string& stage,
                            MonotonicMs last_ok, const char* what) {
    // Checked per stage: a single pass that finds two stalled stages must not
    // spend more than the budget on them.
    if (budget_exhausted()) return;

    // Cooldown. A stage rebuilt at last_restart_ms_[slot] gets the staleness
    // window to recover before it is judged again.
    if (now - last_restart_ms_[slot] < threshold) return;

    if (!stalled(now, last_ok, threshold)) return;

    std::string reason = std::string("no ") + what + " output for " +
                         std::to_string(stall_age_ms(now, last_ok)) + " ms";
    const Result repaired = repair(stage);
    if (!repaired.ok()) {
      // Record the repair's own message. The action is kept and the budget is
      // spent either way: a rebuild that failed is still an attempt, and a
      // permanently broken stage must not be retried on every iteration.
      reason = repaired.message();
    }

    note_restart(now, stage, std::move(reason));
    last_restart_ms_[slot] = now;
    actions.push_back(history_.back());
  };

  // Fixed order — encoder, then labels ascending — keeps history deterministic.
  consider(0, encoder_stage(), stats.last_encode_ms, "encode");
  for (EventType label : kLabels) {
    const size_t index = index_of(label);
    if (!enabled[index]) continue;  // disabled is never a fault
    consider(1 + index, detector_stage(label), stats.last_detection_ms[index],
             "detection");
  }
  return actions;
}

bool Supervisor::budget_exhausted() const {
  // history_ never grows past max_restarts_, so this cast is safe and keeps the
  // comparison free of a signed/unsigned warning. A non-positive budget is spent
  // before it starts.
  return static_cast<int>(history_.size()) >= max_restarts_;
}

void Supervisor::note_restart(MonotonicMs now, std::string stage,
                              std::string reason) {
  RecoveryAction action;
  action.at_ms = now;
  action.stage = std::move(stage);
  action.reason = std::move(reason);
  history_.push_back(std::move(action));
}

const char* Supervisor::encoder_stage() { return "encoder"; }

std::string Supervisor::detector_stage(EventType label) {
  return std::string("detector:") + to_wire(label);
}

}  // namespace hv
