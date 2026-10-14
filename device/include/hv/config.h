#pragma once

// Device configuration: defaults <- JSON file <- HOMEVIDEO_* environment.
//
// Every knob the device needs is here, including the ones the board will only
// be able to answer after the phase-A measurements (quota sizes, detection
// scheduling). Nothing is hard-coded in the modules.

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "hv/status.h"
#include "hv/types.h"

namespace hv {

struct CaptureSpec {
  uint32_t width = 1280;
  uint32_t height = 720;
  uint32_t fps = 15;
  bool realtime = true;
};

struct EncodeSpec {
  std::string codec = "h264";
  uint32_t bitrate_bps = 1500000;
  uint32_t gop_ms = 2000;
};

struct DetectorSpec {
  EventType label = EventType::PersonIntrusion;
  std::string model_path;
  uint32_t input_width = 224;
  uint32_t input_height = 224;
  float conf_threshold = 0.5f;
  std::string model_version = "unset";
  std::vector<SimulatedDetection> simulation;
};

struct DetectSpec {
  std::vector<DetectorSpec> detectors;
  int64_t interval_ms = 200;
  uint32_t confirm_frames = 3;
  int64_t confirm_hold_ms = 1000;
  /** Upper bound on the gap between two runs of the same label (design §9). */
  int64_t schedule_max_gap_ms = 1000;
};

struct EventSpec {
  int64_t pre_roll_ms = 10000;
  int64_t post_roll_ms = 20000;
  int64_t segment_ms = 60000;
  /**
   * Secondary roll trigger so a segment stays bounded in RAM. On the board the
   * segmenter holds one segment while it fills; with a 2 Mbps stream and a 2 s
   * GOP this caps peak use at roughly segment_bytes + one GOP (design §5.2).
   */
  uint64_t segment_bytes = 4ull * 1024 * 1024;
  int64_t merge_window_ms = 30000;
  uint64_t ring_buffer_bytes = 8ull * 1024 * 1024;
};

struct UploadSpec {
  int64_t heartbeat_ms = 10000;
  int64_t arm_poll_ms = 5000;
  int64_t max_backoff_ms = 60000;
  int max_attempts = 8;
  uint64_t rate_bytes_per_s = 256ull * 1024;
  int http_timeout_ms = 10000;
};

struct QuotaSpec {
  uint64_t rolling_bytes = 32ull * 1024 * 1024;
  uint64_t pending_bytes = 256ull * 1024 * 1024;
  uint64_t uploaded_bytes = 512ull * 1024 * 1024;
  uint64_t metadata_bytes = 16ull * 1024 * 1024;
};

struct HealthSpec {
  int64_t video_stale_ms = 5000;
  int64_t recording_stale_ms = 5000;
  int max_stage_restarts = 5;
};

struct DeviceConfig {
  std::string device_id = "v831a";
  std::string hub_url = "http://127.0.0.1:8000";
  std::filesystem::path spool_dir = "spool";
  std::string log_level = "info";

  CaptureSpec capture;
  EncodeSpec encode;
  DetectSpec detect;
  EventSpec event;
  UploadSpec upload;
  QuotaSpec quota;
  HealthSpec health;
};

DeviceConfig default_config();

/**
 * Loads defaults, then the JSON file (skipped when `file` is empty), then the
 * environment overrides, then validates. `env` is an explicit map so tests do
 * not touch the process environment.
 *
 * Returns Code::NotFound when a non-empty path does not exist, Code::Invalid on
 * parse or validation failure (the message names the offending field).
 */
Result load_config(const std::filesystem::path& file,
                   const std::map<std::string, std::string>& env,
                   DeviceConfig& out);

/** Convenience overload reading the real process environment. */
Result load_config(const std::filesystem::path& file, DeviceConfig& out);

Result validate(const DeviceConfig& config);

/** Backend-facing detector configuration for each of the three labels. */
std::vector<DetectorConfig> make_detector_configs(const DeviceConfig& config);

/** Reads a group from a parsed layout safely; returns nullptr when absent. */
const DetectorSpec* find_detector(const DetectSpec& spec, EventType label);

}  // namespace hv
