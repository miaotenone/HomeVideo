#include <filesystem>
#include <fstream>
#include <string>

#include "harness.h"
#include "hv/config.h"

using namespace hv;
namespace fs = std::filesystem;

namespace {

int g_counter = 0;

fs::path write_json(const std::string& body) {
  const fs::path path = fs::temp_directory_path() /
                        ("hv_cfg_" + std::to_string(++g_counter) + ".json");
  std::ofstream out(path, std::ios::trunc);
  out << body;
  out.close();
  return path;
}

bool mentions(const Result& result, const std::string& needle) {
  return result.message().find(needle) != std::string::npos;
}

}  // namespace

HV_TEST(config_defaults_match_the_design) {
  const DeviceConfig config = default_config();
  HV_CHECK_EQ(config.event.pre_roll_ms, int64_t{10000});
  HV_CHECK_EQ(config.event.post_roll_ms, int64_t{20000});
  HV_CHECK_EQ(config.event.segment_ms, int64_t{60000});
  HV_CHECK_EQ(config.upload.heartbeat_ms, int64_t{10000});
  HV_CHECK_EQ(config.upload.arm_poll_ms, int64_t{5000});
  HV_CHECK_EQ(config.capture.width, uint32_t{1280});
  HV_CHECK_EQ(config.capture.height, uint32_t{720});
  HV_CHECK_EQ(config.capture.fps, uint32_t{15});
  HV_CHECK_EQ(config.encode.bitrate_bps, uint32_t{1500000});
  HV_CHECK_EQ(config.detect.detectors.size(), size_t{3});
  HV_CHECK(validate(default_config()).ok());
}

HV_TEST(config_default_detectors_cover_three_labels) {
  const DeviceConfig config = default_config();
  HV_CHECK(find_detector(config.detect, EventType::PersonIntrusion) != nullptr);
  HV_CHECK(find_detector(config.detect, EventType::FireSuspected) != nullptr);
  HV_CHECK(find_detector(config.detect, EventType::SmokeSuspected) != nullptr);
}

HV_TEST(config_file_overrides_defaults_and_env_wins_over_file) {
  const fs::path file = write_json(R"({
    "device_id": "from_file",
    "hub_url": "http://10.0.0.5:8000",
    "capture": {"fps": 20},
    "event": {"segment_ms": 45000}
  })");

  DeviceConfig config;
  Result result = load_config(file, {}, config);
  HV_CHECK(result.ok());
  HV_CHECK_STR_EQ(config.device_id, "from_file");
  HV_CHECK_STR_EQ(config.hub_url, "http://10.0.0.5:8000");
  HV_CHECK_EQ(config.capture.fps, uint32_t{20});
  HV_CHECK_EQ(config.event.segment_ms, int64_t{45000});
  // Untouched fields keep their defaults.
  HV_CHECK_EQ(config.event.pre_roll_ms, int64_t{10000});

  result = load_config(file, {{"HOMEVIDEO_DEVICE_ID", "from_env"},
                             {"HOMEVIDEO_EVENT_SEGMENT_MS", "30000"}},
                       config);
  HV_CHECK(result.ok());
  HV_CHECK_STR_EQ(config.device_id, "from_env");
  HV_CHECK_EQ(config.event.segment_ms, int64_t{30000});
  HV_CHECK_EQ(config.capture.fps, uint32_t{20});
}

HV_TEST(config_unknown_fields_are_ignored) {
  const fs::path file = write_json(R"({"nonsense": 1, "capture": {"fps": 12, "nope": true}})");
  DeviceConfig config;
  const Result result = load_config(file, {}, config);
  HV_CHECK(result.ok());
  HV_CHECK_EQ(config.capture.fps, uint32_t{12});
}

HV_TEST(config_missing_file_is_not_found) {
  DeviceConfig config;
  const Result result =
      load_config(fs::temp_directory_path() / "hv_definitely_missing.json", {}, config);
  HV_CHECK_FALSE(result.ok());
  HV_CHECK_EQ(result.code(), Code::NotFound);
}

HV_TEST(config_invalid_values_name_the_field) {
  struct Case {
    const std::string body;
    const std::string field;
  };
  const Case cases[] = {
      {R"({"capture": {"fps": 0}})", "capture.fps"},
      {R"({"event": {"segment_ms": 10}})", "event.segment_ms"},
      {R"({"event": {"pre_roll_ms": -1}})", "event.pre_roll_ms"},
      {R"({"hub_url": "192.168.1.10:8000"})", "hub_url"},
      {R"({"device_id": "bad id!"})", "device_id"},
      {R"({"upload": {"max_attempts": 0}})", "upload.max_attempts"},
      {R"({"log_level": "loud"})", "log_level"},
  };

  for (const auto& item : cases) {
    DeviceConfig config;
    const Result result = load_config(write_json(item.body), {}, config);
    HV_CHECK_FALSE(result.ok());
    HV_CHECK_EQ(result.code(), Code::Invalid);
    HV_CHECK(mentions(result, item.field));
  }
}

HV_TEST(config_env_invalid_integer_is_rejected) {
  DeviceConfig config;
  const Result result = load_config("", {{"HOMEVIDEO_CAPTURE_FPS", "many"}}, config);
  HV_CHECK_FALSE(result.ok());
  HV_CHECK_EQ(result.code(), Code::Invalid);
  HV_CHECK(mentions(result, "HOMEVIDEO_CAPTURE_FPS"));
}

HV_TEST(config_detectors_must_cover_all_three_labels) {
  // A duplicated label means that label is not present exactly once.
  const std::string duplicated = R"({
    "detect": {"detectors": [
      {"label": "person_intrusion", "model_path": "/m/person.mud"},
      {"label": "person_intrusion", "model_path": "/m/person2.mud"},
      {"label": "fire_suspected", "model_path": "/m/fire.mud"}
    ]}
  })";
  DeviceConfig config;
  Result result = load_config(write_json(duplicated), {}, config);
  HV_CHECK_FALSE(result.ok());
  HV_CHECK_EQ(result.code(), Code::Invalid);
  HV_CHECK(mentions(result, "detect.detectors"));

  // A label that is missing altogether must be named in the message.
  const std::string missing = R"({
    "detect": {"detectors": [
      {"label": "person_intrusion", "model_path": "/m/person.mud"},
      {"label": "fire_suspected", "model_path": "/m/fire.mud"}
    ]}
  })";
  result = load_config(write_json(missing), {}, config);
  HV_CHECK_FALSE(result.ok());
  HV_CHECK_EQ(result.code(), Code::Invalid);
  HV_CHECK(mentions(result, "smoke_suspected"));
}

HV_TEST(config_detector_simulation_is_parsed) {
  const std::string body = R"({
    "detect": {"detectors": [
      {"label": "person_intrusion", "model_path": "/m/person.mud",
       "model_version": "person-yolo-v1",
       "simulation": [{"at_run": 5, "runs": 3, "confidence": 0.8, "bbox": [1,2,3,4], "zone": "door"}]},
      {"label": "fire_suspected", "model_path": "/m/fire.mud"},
      {"label": "smoke_suspected", "model_path": "/m/smoke.mud"}
    ]}
  })";
  DeviceConfig config;
  const Result result = load_config(write_json(body), {}, config);
  HV_CHECK(result.ok());

  const auto detector = make_detector_configs(config);
  HV_CHECK_EQ(detector.size(), size_t{3});
  const DetectorSpec* person =
      find_detector(config.detect, EventType::PersonIntrusion);
  HV_CHECK(person != nullptr);
  HV_CHECK_EQ(person->simulation.size(), size_t{1});
  HV_CHECK_EQ(person->simulation[0].at_run, uint32_t{5});
  HV_CHECK_EQ(person->simulation[0].runs, uint32_t{3});
  HV_CHECK(person->simulation[0].bbox.has_value());
  HV_CHECK_STR_EQ(person->simulation[0].zone.value_or(""), "door");
  HV_CHECK_STR_EQ(person->model_version, "person-yolo-v1");
}

HV_TEST(config_make_detector_configs_carries_label_and_version) {
  DeviceConfig config = default_config();
  for (auto& spec : config.detect.detectors) {
    spec.model_version = std::string("v-") + to_wire(spec.label);
  }
  const auto backends = make_detector_configs(config);
  HV_CHECK_EQ(backends.size(), size_t{3});
  HV_CHECK_EQ(backends[0].label, EventType::PersonIntrusion);
  HV_CHECK_STR_EQ(backends[0].model_version, "v-person_intrusion");
  HV_CHECK_EQ(backends[2].label, EventType::SmokeSuspected);
  HV_CHECK_STR_EQ(backends[2].model_version, "v-smoke_suspected");
}
