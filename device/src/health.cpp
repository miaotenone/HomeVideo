#include "hv/health.h"

#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

#include "json.hpp"

// The staleness rules here mirror hub/README.md "布防与检测语义" exactly, and the
// two traps it calls out drive the design:
//   - a stage that has never run reports null (unknown), never 0/false: 0 in a
//     `*_last_detection_ms` field would read as "ran at boot", which is a
//     different fact from "never ran".
//   - a stalled detector becomes "stale" on the hub side (a gap it derives from
//     our own uptime deltas); there is no failed signal for detection, so we
//     never report one. video_ok / recording_ok are the only hardware flags, and
//     recording staleness is a genuine failure, hence false rather than null.
namespace hv {

namespace {

using nlohmann::json;
namespace fs = std::filesystem;

// Every filesystem call is wrapped so a runtime path returns Result instead of
// throwing (status.h rule of thumb).
Result io_error(const std::string& what) {
  return Result::error(Code::Io, what);
}

}  // namespace

HealthMonitor::HealthMonitor(std::string device_id, std::string boot_id)
    : device_id_(std::move(device_id)), boot_id_(std::move(boot_id)) {
  // -1 is the "never" sentinel; a fresh array is otherwise indeterminate.
  last_detection_ms_.fill(-1);
}

void HealthMonitor::note_frame(MonotonicMs now) { last_frame_ms_ = now; }

void HealthMonitor::note_encode(MonotonicMs now) {
  last_encode_ms_ = now;
  ever_encoded_ = true;
}

void HealthMonitor::note_detection(EventType label, MonotonicMs now) {
  last_detection_ms_[index_of(label)] = now;
}

void HealthMonitor::note_recording_ok(bool ok, MonotonicMs now) {
  recording_ok_ = ok;
  recording_reported_ = true;
  last_recording_ok_ms_ = now;
}

void HealthMonitor::note_storage_free(uint64_t bytes) {
  storage_free_bytes_ = bytes;
}

std::optional<bool> HealthMonitor::video_ok(
    MonotonicMs now, const HealthThresholds& thresholds) const {
  // Before the first chunk there is nothing to call healthy or unhealthy.
  if (!ever_encoded_) return std::nullopt;
  return now - last_encode_ms_ <= thresholds.video_stale_ms;
}

std::optional<bool> HealthMonitor::recording_ok(
    MonotonicMs now, const HealthThresholds& thresholds) const {
  if (!recording_reported_) return std::nullopt;
  // The last report already said failure, or it has gone stale: both are a real
  // recording failure, not an unknown, so this is false rather than nullopt.
  const bool fresh = now - last_recording_ok_ms_ <= thresholds.recording_stale_ms;
  return recording_ok_ && fresh;
}

std::optional<MonotonicMs> HealthMonitor::last_detection_ms(
    EventType label) const {
  const MonotonicMs value = last_detection_ms_[index_of(label)];
  if (value < 0) return std::nullopt;
  return value;
}

DeviceReport HealthMonitor::build(MonotonicMs now,
                                  const HealthThresholds& thresholds,
                                  ArmMode applied_mode, int applied_version,
                                  bool time_trusted) const {
  DeviceReport report;
  report.device_id = device_id_;
  report.boot_id = boot_id_;
  report.uptime_ms = now;
  if (last_frame_ms_ >= 0) report.last_frame_ms = last_frame_ms_;
  if (ever_encoded_) report.last_encode_ms = last_encode_ms_;
  report.person_last_detection_ms = last_detection_ms(EventType::PersonIntrusion);
  report.fire_last_detection_ms = last_detection_ms(EventType::FireSuspected);
  report.smoke_last_detection_ms = last_detection_ms(EventType::SmokeSuspected);
  report.video_ok = video_ok(now, thresholds);
  report.recording_ok = recording_ok(now, thresholds);
  report.storage_free_bytes = storage_free_bytes_;
  report.arm_applied = applied_mode;
  report.arm_applied_version = applied_version;
  report.time_trusted = time_trusted;
  return report;
}

ArmController::ArmController(HubClient& client, IClock& clock, fs::path state_dir)
    : client_(client),
      clock_(clock),
      state_path_(std::move(state_dir) / "device_state.json") {}

Result ArmController::load_persisted() {
  std::error_code ec;
  const bool exists = fs::exists(state_path_, ec);
  if (ec) {
    return io_error("cannot stat " + state_path_.string() + ": " + ec.message());
  }
  // First boot: no state file is not an error, the defaults (unknown/0) stand.
  if (!exists) return Result::success();

  std::string body;
  try {
    std::ifstream in(state_path_, std::ios::binary);
    if (!in.is_open()) {
      return io_error("cannot read " + state_path_.string());
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    body = buffer.str();
  } catch (const std::exception& error) {
    return io_error("read failed for " + state_path_.string() + ": " +
                    error.what());
  }

  // Validate the whole document before mutating anything, so a malformed file
  // leaves the defaults in place instead of a half-restored configuration.
  try {
    const json doc = json::parse(body);
    if (!doc.is_object()) {
      return Result::error(Code::Invalid, "device_state.json must be an object");
    }
    const auto mode_it = doc.find("applied_mode");
    const auto version_it = doc.find("applied_version");
    if (mode_it == doc.end() || !mode_it->is_string() ||
        version_it == doc.end() || !version_it->is_number_integer()) {
      return Result::error(Code::Invalid,
                           "device_state.json needs applied_mode and "
                           "applied_version");
    }
    const auto mode = arm_mode_from_wire(mode_it->get<std::string>());
    if (!mode.has_value()) {
      return Result::error(Code::Invalid,
                           "device_state.json has an unknown applied_mode");
    }
    applied_mode_ = *mode;
    applied_version_ = version_it->get<int>();
    return Result::success();
  } catch (const std::exception& error) {
    return Result::error(Code::Invalid, std::string("device_state.json is not "
                                                    "valid JSON: ") +
                                            error.what());
  }
}

Result ArmController::persist() const {
  try {
    const fs::path dir = state_path_.parent_path();
    if (!dir.empty()) fs::create_directories(dir);

    json doc;
    doc["applied_mode"] = to_wire(applied_mode_);
    doc["applied_version"] = applied_version_;

    // Atomic: write a sibling temp file, then rename over the target, so a crash
    // can never leave a truncated state file behind.
    const fs::path tmp = state_path_.string() + ".tmp";
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      return io_error("cannot open " + tmp.string() + " for writing");
    }
    out << doc.dump(2);
    out.flush();
    if (!out.good()) {
      out.close();
      std::error_code ignored;
      fs::remove(tmp, ignored);
      return io_error("short write to " + tmp.string());
    }
    out.close();
    fs::rename(tmp, state_path_);
    return Result::success();
  } catch (const std::exception& error) {
    return io_error("persist failed for " + state_path_.string() + ": " +
                    error.what());
  }
}

Result ArmController::poll_and_apply() {
  ArmMode mode = ArmMode::Unknown;
  int version = 0;
  const Result polled = client_.get_arm(mode, version);
  last_poll_ms_ = clock_.monotonic_ms();

  if (!polled.ok()) {
    // A failed poll is a transport problem, not a mode change: keep enforcing
    // the last confirmed configuration and acknowledge nothing.
    return polled;
  }

  const bool moved = version > applied_version_;
  // A hub that has never published a version can still hand us a real mode the
  // first time; that transition is applied even though the version is equal.
  const bool first_arm =
      applied_mode_ == ArmMode::Unknown && mode != ArmMode::Unknown;
  if (!moved && !first_arm) {
    return Result::success();
  }

  // Store what the device is now enforcing, unknown included: unknown is a valid
  // applied value and must not be dressed up as an armed mode.
  applied_mode_ = mode;
  applied_version_ = version;

  // design §8: confirm only after applying *and* persisting. If we cannot write
  // the new configuration, an ack would let the hub believe the mode is in force
  // while a restart would silently fall back to the old one.
  const Result persisted = persist();
  if (!persisted.ok()) return persisted;

  const Result acked = client_.ack_arm(applied_mode_, applied_version_);
  if (acked.ok()) {
    acks_sent_++;
  }
  // On ack failure the applied values stay: they describe what the device is
  // really doing, and the next poll will retry the confirmation.
  return acked;
}

}  // namespace hv
