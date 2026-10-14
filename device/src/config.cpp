#include "hv/config.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include "json.hpp"

namespace hv {

namespace {

using nlohmann::json;

const json* group(const json& doc, const char* key) {
  const auto it = doc.find(key);
  if (it == doc.end() || !it->is_object()) return nullptr;
  return &(*it);
}

void read_u32(const json& obj, const char* key, uint32_t& out) {
  const auto it = obj.find(key);
  if (it == obj.end() || !it->is_number_integer()) return;
  const long long value = it->get<long long>();
  if (value >= 0) out = static_cast<uint32_t>(value);
}

void read_int(const json& obj, const char* key, int& out) {
  const auto it = obj.find(key);
  if (it == obj.end() || !it->is_number_integer()) return;
  out = static_cast<int>(it->get<long long>());
}

void read_i64(const json& obj, const char* key, int64_t& out) {
  const auto it = obj.find(key);
  if (it == obj.end() || !it->is_number_integer()) return;
  out = it->get<long long>();
}

void read_u64(const json& obj, const char* key, uint64_t& out) {
  const auto it = obj.find(key);
  if (it == obj.end() || !it->is_number()) return;
  const double value = it->get<double>();
  if (value >= 0) out = static_cast<uint64_t>(value);
}

void read_bool(const json& obj, const char* key, bool& out) {
  const auto it = obj.find(key);
  if (it == obj.end() || !it->is_boolean()) return;
  out = it->get<bool>();
}

void read_float(const json& obj, const char* key, float& out) {
  const auto it = obj.find(key);
  if (it == obj.end() || !it->is_number()) return;
  out = static_cast<float>(it->get<double>());
}

void read_string(const json& obj, const char* key, std::string& out) {
  const auto it = obj.find(key);
  if (it == obj.end() || !it->is_string()) return;
  out = it->get<std::string>();
}

bool parse_i64(const std::string& text, int64_t& out) {
  if (text.empty()) return false;
  errno = 0;
  char* end = nullptr;
  const long long value = std::strtoll(text.c_str(), &end, 10);
  if (errno != 0 || end == text.c_str() || *end != '\0') return false;
  out = static_cast<int64_t>(value);
  return true;
}

bool parse_bool(const std::string& text, bool& out) {
  if (text == "1" || text == "true" || text == "yes" || text == "on") {
    out = true;
    return true;
  }
  if (text == "0" || text == "false" || text == "no" || text == "off") {
    out = false;
    return true;
  }
  return false;
}

template <class T>
Result apply_env_int(const std::map<std::string, std::string>& env,
                     const char* key, T& target) {
  const auto it = env.find(key);
  if (it == env.end()) return Result::success();
  int64_t value = 0;
  if (!parse_i64(it->second, value)) {
    return Result::error(Code::Invalid,
                         std::string(key) + " is not an integer: '" +
                             it->second + "'");
  }
  target = static_cast<T>(value);
  return Result::success();
}

Result apply_env_bool(const std::map<std::string, std::string>& env,
                      const char* key, bool& target) {
  const auto it = env.find(key);
  if (it == env.end()) return Result::success();
  bool value = false;
  if (!parse_bool(it->second, value)) {
    return Result::error(Code::Invalid,
                         std::string(key) + " is not a boolean: '" +
                             it->second + "'");
  }
  target = value;
  return Result::success();
}

Result apply_env_string(const std::map<std::string, std::string>& env,
                        const char* key, std::string& target) {
  const auto it = env.find(key);
  if (it == env.end()) return Result::success();
  target = it->second;
  return Result::success();
}

bool valid_device_id(const std::string& value) {
  if (value.empty() || value.size() > 64) return false;
  for (const char c : value) {
    const bool allowed = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                         (c >= 'A' && c <= 'Z') || c == '.' || c == '_' ||
                         c == '-';
    if (!allowed) return false;
  }
  return true;
}

bool has_http_scheme(const std::string& url) {
  return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
}

bool valid_log_level(const std::string& value) {
  return value == "debug" || value == "info" || value == "warn" ||
         value == "error";
}

Result invalid(const std::string& field, const std::string& why) {
  return Result::error(Code::Invalid, field + ": " + why);
}

}  // namespace

DeviceConfig default_config() {
  DeviceConfig config;
  DetectorSpec person;
  person.label = EventType::PersonIntrusion;
  DetectorSpec fire;
  fire.label = EventType::FireSuspected;
  DetectorSpec smoke;
  smoke.label = EventType::SmokeSuspected;
  config.detect.detectors = {std::move(person), std::move(fire), std::move(smoke)};
  return config;
}

const DetectorSpec* find_detector(const DetectSpec& spec, EventType label) {
  for (const auto& detector : spec.detectors) {
    if (detector.label == label) return &detector;
  }
  return nullptr;
}

std::vector<DetectorConfig> make_detector_configs(const DeviceConfig& config) {
  std::vector<DetectorConfig> out;
  out.reserve(config.detect.detectors.size());
  for (const auto& spec : config.detect.detectors) {
    DetectorConfig entry;
    entry.label = spec.label;
    entry.model_path = spec.model_path;
    entry.input_width = spec.input_width;
    entry.input_height = spec.input_height;
    entry.conf_threshold = spec.conf_threshold;
    entry.model_version = spec.model_version;
    entry.simulation = spec.simulation;
    out.push_back(std::move(entry));
  }
  return out;
}

Result validate(const DeviceConfig& config) {
  if (!valid_device_id(config.device_id)) {
    return invalid("device_id", "must be 1-64 chars of [A-Za-z0-9._-]");
  }
  if (!has_http_scheme(config.hub_url)) {
    return invalid("hub_url", "must start with http:// or https://");
  }
  if (config.spool_dir.empty()) {
    return invalid("spool_dir", "must not be empty");
  }
  if (!valid_log_level(config.log_level)) {
    return invalid("log_level", "must be debug|info|warn|error");
  }

  if (config.capture.width == 0) return invalid("capture.width", "must be > 0");
  if (config.capture.height == 0) return invalid("capture.height", "must be > 0");
  if (config.capture.fps == 0 || config.capture.fps > 60) {
    return invalid("capture.fps", "must be 1..60");
  }

  if (config.encode.codec.empty()) return invalid("encode.codec", "must not be empty");
  if (config.encode.bitrate_bps == 0) {
    return invalid("encode.bitrate_bps", "must be > 0");
  }
  if (config.encode.gop_ms == 0) return invalid("encode.gop_ms", "must be > 0");

  const EventType required[] = {EventType::PersonIntrusion,
                                EventType::FireSuspected,
                                EventType::SmokeSuspected};
  for (const EventType label : required) {
    int seen = 0;
    for (const auto& detector : config.detect.detectors) {
      if (detector.label == label) seen++;
    }
    if (seen != 1) {
      return invalid("detect.detectors",
                     std::string("exactly one detector for '") + to_wire(label) +
                         "' is required, found " + std::to_string(seen));
    }
  }
  for (const auto& detector : config.detect.detectors) {
    if (detector.input_width == 0 || detector.input_height == 0) {
      return invalid("detect.detectors.input_size", "must be > 0");
    }
  }
  if (config.detect.interval_ms <= 0) {
    return invalid("detect.interval_ms", "must be > 0");
  }
  if (config.detect.confirm_frames < 1) {
    return invalid("detect.confirm_frames", "must be >= 1");
  }
  if (config.detect.confirm_hold_ms < 0) {
    return invalid("detect.confirm_hold_ms", "must be >= 0");
  }
  if (config.detect.schedule_max_gap_ms <= 0) {
    return invalid("detect.schedule_max_gap_ms", "must be > 0");
  }

  if (config.event.pre_roll_ms < 0) {
    return invalid("event.pre_roll_ms", "must be >= 0");
  }
  if (config.event.post_roll_ms < 0) {
    return invalid("event.post_roll_ms", "must be >= 0");
  }
  if (config.event.segment_ms < 1000) {
    return invalid("event.segment_ms", "must be >= 1000");
  }
  if (config.event.merge_window_ms < 0) {
    return invalid("event.merge_window_ms", "must be >= 0");
  }
  // An event may not close before its own tail has been recorded; the gap
  // between the two is what folds an intermittent burst into a single event.
  if (config.event.merge_window_ms < config.event.post_roll_ms) {
    return invalid("event.merge_window_ms",
                   "must be >= event.post_roll_ms (the event would close "
                   "before its post-roll tail was recorded)");
  }
  if (config.event.segment_bytes == 0) {
    return invalid("event.segment_bytes", "must be > 0");
  }
  if (config.event.ring_buffer_bytes == 0) {
    return invalid("event.ring_buffer_bytes", "must be > 0");
  }

  if (config.upload.heartbeat_ms < 1000) {
    return invalid("upload.heartbeat_ms", "must be >= 1000");
  }
  if (config.upload.arm_poll_ms < 1000) {
    return invalid("upload.arm_poll_ms", "must be >= 1000");
  }
  if (config.upload.max_backoff_ms < 1000) {
    return invalid("upload.max_backoff_ms", "must be >= 1000");
  }
  if (config.upload.max_attempts < 1 || config.upload.max_attempts > 100) {
    return invalid("upload.max_attempts", "must be 1..100");
  }
  if (config.upload.rate_bytes_per_s == 0) {
    return invalid("upload.rate_bytes_per_s", "must be > 0");
  }
  if (config.upload.http_timeout_ms <= 0) {
    return invalid("upload.http_timeout_ms", "must be > 0");
  }

  if (config.quota.rolling_bytes == 0) {
    return invalid("quota.rolling_bytes", "must be > 0");
  }
  if (config.quota.pending_bytes == 0) {
    return invalid("quota.pending_bytes", "must be > 0");
  }
  if (config.quota.uploaded_bytes == 0) {
    return invalid("quota.uploaded_bytes", "must be > 0");
  }
  if (config.quota.metadata_bytes == 0) {
    return invalid("quota.metadata_bytes", "must be > 0");
  }

  if (config.health.video_stale_ms <= 0) {
    return invalid("health.video_stale_ms", "must be > 0");
  }
  if (config.health.recording_stale_ms <= 0) {
    return invalid("health.recording_stale_ms", "must be > 0");
  }
  if (config.health.max_stage_restarts < 1) {
    return invalid("health.max_stage_restarts", "must be >= 1");
  }

  return Result::success();
}

Result load_config(const std::filesystem::path& file,
                   const std::map<std::string, std::string>& env,
                   DeviceConfig& out) {
  DeviceConfig config = default_config();

  if (!file.empty()) {
    std::ifstream input(file);
    if (!input.is_open()) {
      return Result::error(Code::NotFound,
                           "config file not found: " + file.string());
    }
    json doc;
    try {
      input >> doc;
    } catch (const std::exception& error) {
      return Result::error(Code::Invalid,
                           std::string("config parse error: ") + error.what());
    }
    if (!doc.is_object()) {
      return Result::error(Code::Invalid, "config root must be a JSON object");
    }

    read_string(doc, "device_id", config.device_id);
    read_string(doc, "hub_url", config.hub_url);
    read_string(doc, "log_level", config.log_level);
    std::string spool;
    read_string(doc, "spool_dir", spool);
    if (!spool.empty()) config.spool_dir = spool;

    if (const json* g = group(doc, "capture")) {
      read_u32(*g, "width", config.capture.width);
      read_u32(*g, "height", config.capture.height);
      read_u32(*g, "fps", config.capture.fps);
      read_bool(*g, "realtime", config.capture.realtime);
    }
    if (const json* g = group(doc, "encode")) {
      read_string(*g, "codec", config.encode.codec);
      read_u32(*g, "bitrate_bps", config.encode.bitrate_bps);
      read_u32(*g, "gop_ms", config.encode.gop_ms);
    }
    if (const json* g = group(doc, "detect")) {
      read_i64(*g, "interval_ms", config.detect.interval_ms);
      read_u32(*g, "confirm_frames", config.detect.confirm_frames);
      read_i64(*g, "confirm_hold_ms", config.detect.confirm_hold_ms);
      read_i64(*g, "schedule_max_gap_ms", config.detect.schedule_max_gap_ms);

      const auto detectors = g->find("detectors");
      if (detectors != g->end() && detectors->is_array()) {
        std::vector<DetectorSpec> parsed;
        for (const auto& entry : *detectors) {
          if (!entry.is_object()) {
            return invalid("detect.detectors", "each entry must be an object");
          }
          DetectorSpec spec;
          std::string label;
          read_string(entry, "label", label);
          const auto type = event_type_from_wire(label);
          if (!type.has_value()) {
            return invalid("detect.detectors.label",
                           "unknown event type '" + label + "'");
          }
          spec.label = *type;
          read_string(entry, "model_path", spec.model_path);
          read_u32(entry, "input_width", spec.input_width);
          read_u32(entry, "input_height", spec.input_height);
          read_float(entry, "conf_threshold", spec.conf_threshold);
          read_string(entry, "model_version", spec.model_version);

          const auto simulation = entry.find("simulation");
          if (simulation != entry.end() && simulation->is_array()) {
            for (const auto& item : *simulation) {
              if (!item.is_object()) continue;
              SimulatedDetection script;
              read_u32(item, "at_run", script.at_run);
              read_u32(item, "runs", script.runs);
              read_float(item, "confidence", script.confidence);
              if (item.contains("zone") && item["zone"].is_string()) {
                script.zone = item["zone"].get<std::string>();
              }
              if (item.contains("bbox") && item["bbox"].is_array() &&
                  item["bbox"].size() == 4) {
                BBox box;
                box.x = item["bbox"][0].get<int>();
                box.y = item["bbox"][1].get<int>();
                box.w = item["bbox"][2].get<int>();
                box.h = item["bbox"][3].get<int>();
                script.bbox = box;
              }
              spec.simulation.push_back(std::move(script));
            }
          }
          parsed.push_back(std::move(spec));
        }
        config.detect.detectors = std::move(parsed);
      }
    }
    if (const json* g = group(doc, "event")) {
      read_i64(*g, "pre_roll_ms", config.event.pre_roll_ms);
      read_i64(*g, "post_roll_ms", config.event.post_roll_ms);
      read_i64(*g, "segment_ms", config.event.segment_ms);
      read_u64(*g, "segment_bytes", config.event.segment_bytes);
      read_i64(*g, "merge_window_ms", config.event.merge_window_ms);
      read_u64(*g, "ring_buffer_bytes", config.event.ring_buffer_bytes);
    }
    if (const json* g = group(doc, "upload")) {
      read_i64(*g, "heartbeat_ms", config.upload.heartbeat_ms);
      read_i64(*g, "arm_poll_ms", config.upload.arm_poll_ms);
      read_i64(*g, "max_backoff_ms", config.upload.max_backoff_ms);
      read_int(*g, "max_attempts", config.upload.max_attempts);
      read_u64(*g, "rate_bytes_per_s", config.upload.rate_bytes_per_s);
      read_int(*g, "http_timeout_ms", config.upload.http_timeout_ms);
    }
    if (const json* g = group(doc, "quota")) {
      read_u64(*g, "rolling_bytes", config.quota.rolling_bytes);
      read_u64(*g, "pending_bytes", config.quota.pending_bytes);
      read_u64(*g, "uploaded_bytes", config.quota.uploaded_bytes);
      read_u64(*g, "metadata_bytes", config.quota.metadata_bytes);
    }
    if (const json* g = group(doc, "health")) {
      read_i64(*g, "video_stale_ms", config.health.video_stale_ms);
      read_i64(*g, "recording_stale_ms", config.health.recording_stale_ms);
      read_int(*g, "max_stage_restarts", config.health.max_stage_restarts);
    }
  }

  // Environment overrides win over the file so the field can be corrected
  // without editing what is on the SD card.
  {
    const Result r = apply_env_string(env, "HOMEVIDEO_DEVICE_ID", config.device_id);
    if (!r.ok()) return r;
  }
  {
    const Result r = apply_env_string(env, "HOMEVIDEO_HUB_URL", config.hub_url);
    if (!r.ok()) return r;
  }
  {
    const Result r = apply_env_string(env, "HOMEVIDEO_LOG_LEVEL", config.log_level);
    if (!r.ok()) return r;
  }
  {
    std::string spool = config.spool_dir.string();
    const Result r = apply_env_string(env, "HOMEVIDEO_SPOOL_DIR", spool);
    if (!r.ok()) return r;
    config.spool_dir = spool;
  }
  {
    const Result r = apply_env_bool(env, "HOMEVIDEO_CAPTURE_REALTIME",
                                    config.capture.realtime);
    if (!r.ok()) return r;
  }

#define HV_ENV_INT(key, field)                                     \
  {                                                                \
    const Result r = apply_env_int(env, key, config.field);        \
    if (!r.ok()) return r;                                         \
  }

  HV_ENV_INT("HOMEVIDEO_CAPTURE_WIDTH", capture.width)
  HV_ENV_INT("HOMEVIDEO_CAPTURE_HEIGHT", capture.height)
  HV_ENV_INT("HOMEVIDEO_CAPTURE_FPS", capture.fps)
  HV_ENV_INT("HOMEVIDEO_ENCODE_BITRATE_BPS", encode.bitrate_bps)
  HV_ENV_INT("HOMEVIDEO_ENCODE_GOP_MS", encode.gop_ms)
  HV_ENV_INT("HOMEVIDEO_DETECT_INTERVAL_MS", detect.interval_ms)
  HV_ENV_INT("HOMEVIDEO_DETECT_CONFIRM_FRAMES", detect.confirm_frames)
  HV_ENV_INT("HOMEVIDEO_DETECT_CONFIRM_HOLD_MS", detect.confirm_hold_ms)
  HV_ENV_INT("HOMEVIDEO_EVENT_PRE_ROLL_MS", event.pre_roll_ms)
  HV_ENV_INT("HOMEVIDEO_EVENT_POST_ROLL_MS", event.post_roll_ms)
  HV_ENV_INT("HOMEVIDEO_EVENT_SEGMENT_MS", event.segment_ms)
  HV_ENV_INT("HOMEVIDEO_EVENT_SEGMENT_BYTES", event.segment_bytes)
  HV_ENV_INT("HOMEVIDEO_EVENT_MERGE_WINDOW_MS", event.merge_window_ms)
  HV_ENV_INT("HOMEVIDEO_EVENT_RING_BUFFER_BYTES", event.ring_buffer_bytes)
  HV_ENV_INT("HOMEVIDEO_UPLOAD_HEARTBEAT_MS", upload.heartbeat_ms)
  HV_ENV_INT("HOMEVIDEO_UPLOAD_ARM_POLL_MS", upload.arm_poll_ms)
  HV_ENV_INT("HOMEVIDEO_UPLOAD_MAX_BACKOFF_MS", upload.max_backoff_ms)
  HV_ENV_INT("HOMEVIDEO_UPLOAD_MAX_ATTEMPTS", upload.max_attempts)
  HV_ENV_INT("HOMEVIDEO_UPLOAD_RATE_BYTES_PER_S", upload.rate_bytes_per_s)
  HV_ENV_INT("HOMEVIDEO_UPLOAD_HTTP_TIMEOUT_MS", upload.http_timeout_ms)
  HV_ENV_INT("HOMEVIDEO_QUOTA_ROLLING_BYTES", quota.rolling_bytes)
  HV_ENV_INT("HOMEVIDEO_QUOTA_PENDING_BYTES", quota.pending_bytes)
  HV_ENV_INT("HOMEVIDEO_QUOTA_UPLOADED_BYTES", quota.uploaded_bytes)
  HV_ENV_INT("HOMEVIDEO_QUOTA_METADATA_BYTES", quota.metadata_bytes)
  HV_ENV_INT("HOMEVIDEO_HEALTH_VIDEO_STALE_MS", health.video_stale_ms)
  HV_ENV_INT("HOMEVIDEO_HEALTH_RECORDING_STALE_MS", health.recording_stale_ms)
  HV_ENV_INT("HOMEVIDEO_HEALTH_MAX_STAGE_RESTARTS", health.max_stage_restarts)

#undef HV_ENV_INT

  const Result valid = validate(config);
  if (!valid.ok()) return valid;

  out = std::move(config);
  return Result::success();
}

Result load_config(const std::filesystem::path& file, DeviceConfig& out) {
  return load_config(file, {}, out);
}

}  // namespace hv
