#pragma once

// Health monitoring, hub reporting and arm-mode control.
//
// The field names and the staleness rules must match the hub exactly
// (hub/README.md "布防与检测语义"), including the two easy-to-get-wrong cases:
//   - a detector that has never run reports null, never 0 or false
//   - "detection is broken" is expressed as stale, never as failed

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "hv/clock.h"
#include "hv/hub_client.h"
#include "hv/status.h"
#include "hv/types.h"

namespace hv {

struct HealthThresholds {
  MonotonicMs video_stale_ms = 5000;
  MonotonicMs recording_stale_ms = 5000;
};

class HealthMonitor {
 public:
  HealthMonitor(std::string device_id, std::string boot_id);

  void note_frame(MonotonicMs now);
  void note_encode(MonotonicMs now);
  void note_detection(EventType label, MonotonicMs now);
  void note_recording_ok(bool ok, MonotonicMs now);
  void note_storage_free(uint64_t bytes);

  /** nullopt when the stage has never produced anything yet. */
  std::optional<bool> video_ok(MonotonicMs now, const HealthThresholds& thresholds) const;
  std::optional<bool> recording_ok(MonotonicMs now, const HealthThresholds& thresholds) const;
  std::optional<MonotonicMs> last_detection_ms(EventType label) const;

  DeviceReport build(MonotonicMs now, const HealthThresholds& thresholds,
                     ArmMode applied_mode, int applied_version,
                     bool time_trusted) const;

 private:
  std::string device_id_;
  std::string boot_id_;
  MonotonicMs last_frame_ms_ = -1;
  MonotonicMs last_encode_ms_ = -1;
  std::array<MonotonicMs, kEventTypeCount> last_detection_ms_;
  bool ever_encoded_ = false;
  bool recording_ok_ = true;
  bool recording_reported_ = false;
  MonotonicMs last_recording_ok_ms_ = -1;
  std::optional<uint64_t> storage_free_bytes_;
};

/**
 * Applies the hub's desired arm configuration.
 *
 * The applied mode is persisted so a restart keeps enforcing the last confirmed
 * configuration (design §7.2), and `unknown` is only ever the initial state —
 * the device never reports an unacknowledged mode as applied.
 */
class ArmController {
 public:
  ArmController(HubClient& client, IClock& clock, std::filesystem::path state_dir);

  Result load_persisted();
  Result persist() const;

  /** Pulls GET /arm; applies and acknowledges when the version moved. */
  Result poll_and_apply();

  ArmMode applied_mode() const { return applied_mode_; }
  int applied_version() const { return applied_version_; }
  bool should_run_person_detection() const { return applied_mode_ == ArmMode::Away; }
  uint64_t acks_sent() const { return acks_sent_; }
  MonotonicMs last_poll_ms() const { return last_poll_ms_; }
  void set_last_poll_ms(MonotonicMs value) { last_poll_ms_ = value; }

 private:
  HubClient& client_;
  IClock& clock_;
  std::filesystem::path state_path_;
  ArmMode applied_mode_ = ArmMode::Unknown;
  int applied_version_ = 0;
  uint64_t acks_sent_ = 0;
  MonotonicMs last_poll_ms_ = -1;
};

}  // namespace hv
