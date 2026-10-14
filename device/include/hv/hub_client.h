#pragma once

// Hub protocol client: pure mapping between typed structs and the wire format
// documented in hub/README.md. No retry policy and no sequencing live here —
// that is the Uploader's job — so this layer stays easy to test against a stub
// server.
//
// The hub has no authentication yet (hub/README.md "known gaps"), so requests
// are plain HTTP. If/when a token lands, it is added in exactly one place.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "hv/status.h"
#include "hv/types.h"

namespace hv {

/** Mirrors the hub's DeviceReport. Absent optionals serialise as null. */
struct DeviceReport {
  std::string device_id;
  std::string boot_id;
  std::optional<int64_t> uptime_ms;
  std::optional<int64_t> last_frame_ms;
  std::optional<int64_t> last_encode_ms;
  std::optional<int64_t> person_last_detection_ms;
  std::optional<int64_t> fire_last_detection_ms;
  std::optional<int64_t> smoke_last_detection_ms;
  std::optional<bool> video_ok;
  std::optional<bool> recording_ok;
  std::optional<uint64_t> storage_free_bytes;
  std::optional<ArmMode> arm_applied;
  std::optional<int> arm_applied_version;
  bool time_trusted = false;
};

/** Mirrors the hub's EventCreate. */
struct EventMetadata {
  std::string event_id;
  std::string device_id;
  std::string event_type;  // wire spelling
  std::optional<double> started_at;
  bool time_trusted = false;
  std::optional<std::string> device_boot_id;
  std::optional<int64_t> device_started_ms;
  std::string model_version;
  int config_version = 0;
  std::string arm_mode;  // wire spelling
  std::vector<Detection> detections;
};

/** Mirrors the hub's EventClose. */
struct EventClosePayload {
  std::optional<double> ended_at;
  std::optional<int64_t> duration_ms;
  bool time_trusted = false;
};

enum class UploadOutcome {
  Ok,
  Duplicate,
  Conflict,
  EventMissing,
  ChecksumMismatch,
  InsufficientStorage,
  Invalid,
  NetworkError,
  ServerError,
};

const char* to_string(UploadOutcome outcome);

struct HeartbeatResult {
  Result result;
  /** From the response `Date` header: the hub returns no explicit time field. */
  std::optional<int64_t> server_unix;
};

struct EventUpsertResult {
  Result result;
  bool created = false;
};

/** RFC 7231 HTTP-date -> unix seconds. Empty on anything unparseable. */
std::optional<int64_t> parse_http_date(const std::string& value);

/** Serialised DeviceReport JSON; exposed so tests can assert the wire shape. */
std::string device_report_to_json(const DeviceReport& report);
/** Serialised EventCreate JSON. */
std::string event_metadata_to_json(const EventMetadata& metadata);
/** Serialised EventClose JSON. */
std::string event_close_to_json(const EventClosePayload& payload);

class HubClient {
 public:
  HubClient(std::string base_url, std::string device_id, int timeout_ms);

  HeartbeatResult heartbeat(const DeviceReport& report);
  Result get_arm(ArmMode& mode, int& config_version);
  Result ack_arm(ArmMode applied_mode, int applied_version);
  EventUpsertResult create_event(const EventMetadata& metadata);
  UploadOutcome upload_snapshot(const std::string& event_id,
                                const std::filesystem::path& path,
                                const std::string& content_type);
  UploadOutcome upload_segment(const std::string& event_id, int seq,
                               const std::filesystem::path& path,
                               const std::string& sha256_hex);
  Result close_event(const std::string& event_id, const EventClosePayload& payload);
  Result cleanup();

  /** Server time observed on the most recent response, if it carried a Date. */
  std::optional<int64_t> last_server_unix() const;

  const std::string& last_error() const;
  std::string base_url() const;
  const std::string& device_id() const;

 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace hv
