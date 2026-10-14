// Tests for Supervisor, the stalled-stage repair policy.
//
// The cases below are written one fault per case on purpose: each starts from a
// fully healthy PipelineStats and breaks exactly one field, so a failure names
// the exact rule that broke. Three of them encode the traps from supervisor.h:
//   - a disabled stage is never a fault (person detection is off unless away)
//   - a rebuilt stage gets the staleness window again (cooldown)
//   - a failed repair is still recorded and still spends budget

#include <array>
#include <string>
#include <vector>

#include "harness.h"
#include "hv/supervisor.h"

using namespace hv;

namespace {

constexpr MonotonicMs kThreshold = 5000;

HealthThresholds stale_after_5s() {
  HealthThresholds thresholds;
  thresholds.video_stale_ms = kThreshold;
  return thresholds;
}

// Every stage fresh at `now`. Each case then breaks exactly one field.
PipelineStats healthy_stats(MonotonicMs now) {
  PipelineStats stats;
  stats.last_frame_ms = now;
  stats.last_encode_ms = now - 100;
  stats.last_detection_ms.fill(now - 100);
  return stats;
}

// Encoder silent since boot with every detector fresh at `now`.
PipelineStats encoder_dead_stats(MonotonicMs now) {
  PipelineStats stats = healthy_stats(now);
  stats.last_encode_ms = 0;
  return stats;
}

std::array<bool, kEventTypeCount> all_enabled() {
  return {{true, true, true}};
}

// Records the stage names handed to repair and returns a scripted Result.
struct RepairSpy {
  std::vector<std::string> calls;
  Result result = Result::success();

  Supervisor::Repair cb() {
    return [this](const std::string& stage) {
      calls.push_back(stage);
      return result;
    };
  }
};

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace

HV_TEST(supervisor_healthy_pipeline_is_left_alone) {
  Supervisor supervisor(stale_after_5s(), 5);
  RepairSpy spy;

  const std::vector<RecoveryAction> actions =
      supervisor.check(60000, healthy_stats(60000), all_enabled(), spy.cb());

  HV_CHECK(actions.empty());
  HV_CHECK(spy.calls.empty());
  HV_CHECK_EQ(supervisor.history().size(), size_t{0});
  HV_CHECK_EQ(supervisor.restart_count(), uint64_t{0});
  HV_CHECK_FALSE(supervisor.budget_exhausted());
}

HV_TEST(supervisor_stalled_encoder_is_rebuilt_once_then_cooldown) {
  Supervisor supervisor(stale_after_5s(), 5);
  RepairSpy spy;

  const std::vector<RecoveryAction> first =
      supervisor.check(6000, encoder_dead_stats(6000), all_enabled(), spy.cb());
  HV_CHECK_EQ(first.size(), size_t{1});
  HV_CHECK_STR_EQ(first.at(0).stage, "encoder");
  HV_CHECK_EQ(first.at(0).at_ms, MonotonicMs{6000});
  HV_CHECK_TRUE(contains(first.at(0).reason, "no encode output"));
  HV_CHECK_EQ(spy.calls.size(), size_t{1});
  HV_CHECK_EQ(supervisor.history().at(0).stage, std::string("encoder"));

  // Same instant, still stalled: the cooldown must stop a second rebuild.
  const std::vector<RecoveryAction> second =
      supervisor.check(6000, encoder_dead_stats(6000), all_enabled(), spy.cb());
  HV_CHECK(second.empty());
  HV_CHECK_EQ(spy.calls.size(), size_t{1});
  HV_CHECK_EQ(supervisor.restart_count(), uint64_t{1});
}

HV_TEST(supervisor_encoder_rebuilt_again_after_cooldown) {
  Supervisor supervisor(stale_after_5s(), 5);
  RepairSpy spy;

  HV_CHECK_EQ(
      supervisor.check(6000, encoder_dead_stats(6000), all_enabled(), spy.cb())
          .size(),
      size_t{1});

  // 5 s later the cooldown has elapsed and the encoder is still silent, so it is
  // a candidate again.
  const std::vector<RecoveryAction> later =
      supervisor.check(11000, encoder_dead_stats(11000), all_enabled(), spy.cb());
  HV_CHECK_EQ(later.size(), size_t{1});
  HV_CHECK_STR_EQ(later.at(0).stage, "encoder");
  HV_CHECK_EQ(supervisor.restart_count(), uint64_t{2});
}

HV_TEST(supervisor_recovered_stage_is_not_rebuilt_again) {
  Supervisor supervisor(stale_after_5s(), 5);
  RepairSpy spy;

  HV_CHECK_EQ(
      supervisor.check(6000, encoder_dead_stats(6000), all_enabled(), spy.cb())
          .size(),
      size_t{1});

  // The rebuild worked: encode output resumes well inside the window.
  PipelineStats recovered = healthy_stats(12000);
  recovered.last_encode_ms = 11500;
  HV_CHECK(
      supervisor.check(12000, recovered, all_enabled(), spy.cb()).empty());
  HV_CHECK_EQ(supervisor.restart_count(), uint64_t{1});

  // Still fresh a little later (gap 4500 <= 5000): left alone.
  PipelineStats still_fresh = healthy_stats(16000);
  still_fresh.last_encode_ms = 11500;
  HV_CHECK(
      supervisor.check(16000, still_fresh, all_enabled(), spy.cb()).empty());
  HV_CHECK_EQ(supervisor.restart_count(), uint64_t{1});
}

HV_TEST(supervisor_never_encoded_is_grace_then_fault) {
  Supervisor supervisor(stale_after_5s(), 5);
  RepairSpy spy;

  PipelineStats early = healthy_stats(4000);
  early.last_encode_ms = -1;  // never encoded
  // Before the grace period elapses a never-encoded stage is not a fault.
  HV_CHECK(supervisor.check(4000, early, all_enabled(), spy.cb()).empty());
  HV_CHECK_EQ(supervisor.restart_count(), uint64_t{0});

  PipelineStats late = healthy_stats(6000);
  late.last_encode_ms = -1;
  const std::vector<RecoveryAction> actions =
      supervisor.check(6000, late, all_enabled(), spy.cb());
  HV_CHECK_EQ(actions.size(), size_t{1});
  HV_CHECK_STR_EQ(actions.at(0).stage, "encoder");
}

HV_TEST(supervisor_disabled_detector_is_never_a_fault) {
  Supervisor supervisor(stale_after_5s(), 5);
  RepairSpy spy;

  PipelineStats stats = healthy_stats(6000);
  stats.last_detection_ms[index_of(EventType::PersonIntrusion)] = -1;  // never ran
  const std::array<bool, kEventTypeCount> person_off{{false, true, true}};

  // Disabled: "never ran" must not trigger a rebuild.
  HV_CHECK(supervisor.check(6000, stats, person_off, spy.cb()).empty());
  HV_CHECK(spy.calls.empty());

  // Arming away enables it; the same never-ran stage is now a candidate.
  const std::vector<RecoveryAction> actions =
      supervisor.check(6000, stats, all_enabled(), spy.cb());
  HV_CHECK_EQ(actions.size(), size_t{1});
  HV_CHECK_STR_EQ(actions.at(0).stage, "detector:person_intrusion");
}

HV_TEST(supervisor_detectors_are_checked_independently) {
  Supervisor supervisor(stale_after_5s(), 5);
  RepairSpy spy;

  PipelineStats stats = healthy_stats(6000);
  stats.last_detection_ms[index_of(EventType::SmokeSuspected)] = 0;  // only smoke

  const std::vector<RecoveryAction> actions =
      supervisor.check(6000, stats, all_enabled(), spy.cb());
  HV_CHECK_EQ(actions.size(), size_t{1});
  HV_CHECK_STR_EQ(actions.at(0).stage, "detector:smoke_suspected");
  HV_CHECK_EQ(spy.calls.size(), size_t{1});
  HV_CHECK_STR_EQ(spy.calls.at(0), "detector:smoke_suspected");
}

HV_TEST(supervisor_two_stalls_are_ordered_encoder_then_label) {
  Supervisor supervisor(stale_after_5s(), 5);
  RepairSpy spy;

  PipelineStats stats = healthy_stats(6000);
  stats.last_encode_ms = 0;
  stats.last_detection_ms[index_of(EventType::FireSuspected)] = 0;

  const std::vector<RecoveryAction> actions =
      supervisor.check(6000, stats, all_enabled(), spy.cb());
  HV_CHECK_EQ(actions.size(), size_t{2});
  HV_CHECK_STR_EQ(actions.at(0).stage, "encoder");
  HV_CHECK_STR_EQ(actions.at(1).stage, "detector:fire_suspected");
  HV_CHECK_EQ(spy.calls.size(), size_t{2});
}

HV_TEST(supervisor_budget_exhausts_after_max_restarts) {
  Supervisor supervisor(stale_after_5s(), 2);
  RepairSpy spy;

  HV_CHECK_EQ(
      supervisor.check(6000, encoder_dead_stats(6000), all_enabled(), spy.cb())
          .size(),
      size_t{1});
  HV_CHECK_EQ(
      supervisor.check(11000, encoder_dead_stats(11000), all_enabled(), spy.cb())
          .size(),
      size_t{1});
  HV_CHECK_TRUE(supervisor.budget_exhausted());

  // Third stall: the budget is spent, so nothing happens and repair is not even
  // called.
  const std::vector<RecoveryAction> third =
      supervisor.check(16000, encoder_dead_stats(16000), all_enabled(), spy.cb());
  HV_CHECK(third.empty());
  HV_CHECK_EQ(spy.calls.size(), size_t{2});
  HV_CHECK_EQ(supervisor.restart_count(), uint64_t{2});
}

HV_TEST(supervisor_failed_repair_is_recorded_and_spends_budget) {
  Supervisor supervisor(stale_after_5s(), 1);
  RepairSpy spy;
  spy.result = Result::error(Code::Internal, "boom");

  const std::vector<RecoveryAction> actions =
      supervisor.check(6000, encoder_dead_stats(6000), all_enabled(), spy.cb());
  HV_CHECK_EQ(actions.size(), size_t{1});
  HV_CHECK_TRUE(contains(actions.at(0).reason, "boom"));
  // The failure still spent the only restart, so a broken stage cannot loop.
  HV_CHECK_TRUE(supervisor.budget_exhausted());
  HV_CHECK_EQ(supervisor.restart_count(), uint64_t{1});
}

HV_TEST(supervisor_stage_name_spellings) {
  HV_CHECK_STR_EQ(Supervisor::encoder_stage(), "encoder");
  HV_CHECK_STR_EQ(Supervisor::detector_stage(EventType::PersonIntrusion),
                  "detector:person_intrusion");
  HV_CHECK_STR_EQ(Supervisor::detector_stage(EventType::FireSuspected),
                  "detector:fire_suspected");
  HV_CHECK_STR_EQ(Supervisor::detector_stage(EventType::SmokeSuspected),
                  "detector:smoke_suspected");
}
