// End-to-end coverage for App: the wiring of config -> spool -> pipeline ->
// events -> upload against an in-process stub hub.
//
// The stub records the ordered (method, path) pairs it received, which is what
// lets the tests assert upload ordering (metadata -> snapshot -> segment ->
// close) without reaching into the App.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "harness.h"
#include "httplib.h"
#include "hv/app.h"
#include "hv/clock.h"
#include "hv/config.h"
#include "hv/log.h"
#include "hv/spool.h"
#include "json.hpp"

using namespace hv;
namespace fs = std::filesystem;

namespace {

// One recorded request, used for the ordering assertions.
struct Seen {
  std::string method;
  std::string path;
};

bool ends_with(const std::string& text, const std::string& suffix) {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

int index_of(const std::vector<Seen>& seen,
             const std::function<bool(const Seen&)>& pred) {
  for (size_t i = 0; i < seen.size(); ++i) {
    if (pred(seen[i])) return static_cast<int>(i);
  }
  return -1;
}

// A stub hub on an ephemeral port. Mirrors test_hub_client.cpp's RAII shutdown
// helper so a failing HV_CHECK still stops the listener and joins the thread.
class StubHub {
 public:
  StubHub() {
    server_.Post("/api/v1/device/heartbeat",
                 [this](const httplib::Request& req, httplib::Response& res) {
                   record(req);
                   {
                     std::lock_guard<std::mutex> lock(mutex_);
                     heartbeats_.push_back(req.body);
                   }
                   res.status = 200;
                   res.set_content("{\"device_id\":\"dev-a\",\"status\":\"ok\"}",
                                   "application/json");
                 });

    server_.Get("/api/v1/arm",
                [this](const httplib::Request& req, httplib::Response& res) {
                  record(req);
                  std::string mode;
                  int version = 0;
                  {
                    std::lock_guard<std::mutex> lock(mutex_);
                    mode = arm_mode_;
                    version = arm_version_;
                  }
                  res.status = 200;
                  res.set_content("{\"mode\":\"" + mode +
                                      "\",\"config_version\":" +
                                      std::to_string(version) + "}",
                                  "application/json");
                });

    server_.Post("/api/v1/arm/ack",
                 [this](const httplib::Request& req, httplib::Response& res) {
                   record(req);
                   {
                     std::lock_guard<std::mutex> lock(mutex_);
                     arm_acks_.push_back(req.body);
                   }
                   res.status = 200;
                   res.set_content("{\"status\":\"ok\",\"effective\":true}",
                                   "application/json");
                 });

    server_.Post("/api/v1/events",
                 [this](const httplib::Request& req, httplib::Response& res) {
                   record(req);
                   {
                     std::lock_guard<std::mutex> lock(mutex_);
                     event_bodies_.push_back(req.body);
                   }
                   res.status = 201;
                   res.set_content("{\"created\":true}", "application/json");
                 });

    server_.Post("/api/v1/events/:event_id/snapshot",
                 [this](const httplib::Request& req, httplib::Response& res) {
                   record(req);
                   res.status = 201;
                   res.set_content("{\"status\":\"stored\"}", "application/json");
                 });

    server_.Put("/api/v1/events/:event_id/segments/:seq",
                [this](const httplib::Request& req, httplib::Response& res) {
                  record(req);
                  res.status = 201;
                  res.set_content("{\"status\":\"stored\"}", "application/json");
                });

    server_.Post("/api/v1/events/:event_id/close",
                 [this](const httplib::Request& req, httplib::Response& res) {
                   record(req);
                   res.status = 200;
                   res.set_content("{\"status\":\"closed\"}", "application/json");
                 });

    port_ = server_.bind_to_any_port("127.0.0.1");
    thread_ = std::thread([this] { server_.listen_after_bind(); });
    server_.wait_until_ready();
  }

  ~StubHub() {
    server_.stop();
    if (thread_.joinable()) thread_.join();
  }

  StubHub(const StubHub&) = delete;
  StubHub& operator=(const StubHub&) = delete;

  std::string url() const {
    return "http://127.0.0.1:" + std::to_string(port_);
  }

  void set_arm(const std::string& mode, int version) {
    std::lock_guard<std::mutex> lock(mutex_);
    arm_mode_ = mode;
    arm_version_ = version;
  }

  std::vector<Seen> requests() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return requests_;
  }

  std::vector<std::string> heartbeats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return heartbeats_;
  }

  std::vector<std::string> arm_acks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return arm_acks_;
  }

  std::vector<std::string> event_bodies() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return event_bodies_;
  }

 private:
  void record(const httplib::Request& req) {
    std::lock_guard<std::mutex> lock(mutex_);
    requests_.push_back({req.method, req.path});
  }

  httplib::Server server_;
  std::thread thread_;
  int port_ = 0;

  mutable std::mutex mutex_;
  std::vector<Seen> requests_;
  std::vector<std::string> heartbeats_;
  std::vector<std::string> arm_acks_;
  std::vector<std::string> event_bodies_;
  std::string arm_mode_ = "home";
  int arm_version_ = 0;
};

// Removes its tree on destruction so a failing HV_CHECK still cleans up.
class TempDir {
 public:
  explicit TempDir(const std::string& prefix) {
    static std::atomic<int> counter{0};
    root_ = fs::temp_directory_path() /
            (prefix + "-" + new_boot_id() + "-" +
             std::to_string(counter.fetch_add(1)));
    fs::create_directories(root_);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(root_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const fs::path& path() const { return root_; }

 private:
  fs::path root_;
};

// Small windows so a single --once run yields a closed, uploadable event.
DeviceConfig app_config(const fs::path& spool_dir) {
  DeviceConfig config = default_config();
  config.device_id = "dev-app";
  config.spool_dir = spool_dir;
  config.log_level = "error";  // keep the suite output readable
  config.capture.width = 320;
  config.capture.height = 240;
  config.capture.fps = 15;
  config.capture.realtime = false;
  config.encode.codec = "h264";
  config.encode.bitrate_bps = 500000;
  config.encode.gop_ms = 1000;
  config.detect.interval_ms = 50;
  config.detect.confirm_frames = 1;
  config.detect.confirm_hold_ms = 0;
  config.event.pre_roll_ms = 200;
  config.event.post_roll_ms = 200;
  config.event.merge_window_ms = 1000;
  config.event.segment_ms = 1000;
  config.event.ring_buffer_bytes = 1024 * 1024;
  config.upload.heartbeat_ms = 1000;
  config.upload.arm_poll_ms = 1000;
  config.upload.max_backoff_ms = 2000;
  config.upload.max_attempts = 3;
  config.upload.rate_bytes_per_s = 10ull * 1024 * 1024;
  config.upload.http_timeout_ms = 2000;
  return config;
}

// Fire is never gated by arm mode, so a fire script drives an event in every
// mode.
SimulatedDetection fire_script() {
  SimulatedDetection script;
  script.at_run = 1;
  script.runs = 2;
  script.confidence = 0.93f;
  return script;
}

std::vector<fs::path> event_dirs(const fs::path& spool_dir) {
  std::vector<fs::path> dirs;
  const fs::path events = spool_dir / "events";
  std::error_code ec;
  if (!fs::is_directory(events, ec) || ec) return dirs;
  for (const auto& entry : fs::directory_iterator(events, ec)) {
    std::error_code entry_ec;
    if (entry.is_directory(entry_ec) && !entry_ec) dirs.push_back(entry.path());
  }
  return dirs;
}

bool read_bytes(const fs::path& path, std::vector<uint8_t>& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) return false;
  out.assign(std::istreambuf_iterator<char>(in),
             std::istreambuf_iterator<char>());
  return true;
}

bool is_jpeg(const std::vector<uint8_t>& bytes) {
  return bytes.size() >= 2 && bytes[0] == 0xff && bytes[1] == 0xd8;
}

bool is_hex8(const std::string& text) {
  if (text.size() != 8) return false;
  for (const char c : text) {
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    if (!hex) return false;
  }
  return true;
}

}  // namespace

HV_TEST(app_once_uploads_a_complete_event_end_to_end) {
  TempDir dir("hv_app_e2e");
  StubHub hub;

  DeviceConfig config = app_config(dir.path());
  config.hub_url = hub.url();
  config.detect.detectors[1].simulation = {fire_script()};

  RunOptions options;
  options.once = true;
  options.once_budget_ms = 6000;
  App app(config, std::make_unique<SystemClock>());
  HV_CHECK_EQ(app.run(options), 0);

  const std::vector<fs::path> dirs = event_dirs(dir.path());
  HV_CHECK_EQ(dirs.size(), size_t{1});
  if (dirs.empty()) return;
  const fs::path event_dir = dirs.front();

  Manifest manifest;
  HV_CHECK(Manifest::load(event_dir / "manifest.json", manifest).ok());
  HV_CHECK_STR_EQ(manifest.state, "closed");

  std::vector<uint8_t> snapshot;
  HV_CHECK(read_bytes(event_dir / "snapshot.jpg", snapshot));
  HV_CHECK(is_jpeg(snapshot));

  const std::vector<Seen> seen = hub.requests();
  const int meta = index_of(seen, [](const Seen& r) {
    return r.method == "POST" && r.path == "/api/v1/events";
  });
  const int snap = index_of(seen, [](const Seen& r) {
    return r.method == "POST" && ends_with(r.path, "/snapshot");
  });
  const int seg = index_of(seen, [](const Seen& r) {
    return r.method == "PUT" && ends_with(r.path, "/segments/0");
  });
  const int close = index_of(seen, [](const Seen& r) {
    return r.method == "POST" && ends_with(r.path, "/close");
  });
  HV_CHECK(meta >= 0);
  HV_CHECK(snap > meta);
  HV_CHECK(seg > snap);
  HV_CHECK(close > seg);
}

HV_TEST(app_once_with_unreachable_hub_keeps_the_event_on_disk) {
  TempDir dir("hv_app_offline");

  DeviceConfig config = app_config(dir.path());
  // Port 1 has no listener, so every hub call is refused immediately.
  config.hub_url = "http://127.0.0.1:1";
  config.detect.detectors[1].simulation = {fire_script()};

  RunOptions options;
  options.once = true;
  options.once_budget_ms = 2500;
  App app(config, std::make_unique<SystemClock>());
  HV_CHECK_EQ(app.run(options), 0);

  const std::vector<fs::path> dirs = event_dirs(dir.path());
  HV_CHECK_EQ(dirs.size(), size_t{1});
  if (dirs.empty()) return;
  const fs::path event_dir = dirs.front();

  Manifest manifest;
  HV_CHECK(Manifest::load(event_dir / "manifest.json", manifest).ok());
  HV_CHECK_STR_EQ(manifest.state, "closed");
  HV_CHECK_FALSE(manifest.meta_uploaded);
  HV_CHECK_FALSE(manifest.close_uploaded);

  // Every file is retained: nothing was deleted because nothing was confirmed.
  std::vector<uint8_t> snapshot;
  HV_CHECK(read_bytes(event_dir / "snapshot.jpg", snapshot));
  HV_CHECK(is_jpeg(snapshot));
  HV_CHECK(fs::exists(event_dir / "manifest.json"));
  if (!manifest.segments.empty()) {
    HV_CHECK(fs::exists(event_dir / manifest.segments.front().file));
  }
}

HV_TEST(app_applies_arm_mode_from_the_hub_and_acknowledges_it) {
  TempDir dir("hv_app_arm");
  StubHub hub;
  hub.set_arm("away", 3);

  DeviceConfig config = app_config(dir.path());
  config.hub_url = hub.url();
  // Person only runs while armed away, so a person event proves the applied
  // mode actually reached the pipeline.
  config.detect.detectors[0].simulation = {fire_script()};

  RunOptions options;
  options.once = true;
  options.once_budget_ms = 3000;
  App app(config, std::make_unique<SystemClock>());
  HV_CHECK_EQ(app.run(options), 0);

  const std::vector<std::string> acks = hub.arm_acks();
  HV_CHECK_FALSE(acks.empty());
  if (!acks.empty()) {
    const nlohmann::json body = nlohmann::json::parse(acks.front());
    HV_CHECK_STR_EQ(body.at("applied_mode").get<std::string>(), "away");
    HV_CHECK_EQ(body.at("applied_version").get<int>(), 3);
  }

  bool saw_person = false;
  for (const std::string& text : hub.event_bodies()) {
    const nlohmann::json body = nlohmann::json::parse(text);
    if (body.value("event_type", std::string()) == "person_intrusion") {
      saw_person = true;
    }
  }
  HV_CHECK(saw_person);
}

HV_TEST(app_heartbeat_reports_device_id_and_an_eight_hex_boot_id) {
  TempDir dir("hv_app_heartbeat");
  StubHub hub;

  DeviceConfig config = app_config(dir.path());
  config.hub_url = hub.url();
  config.device_id = "dev-app";

  RunOptions options;
  options.once = true;
  options.once_budget_ms = 1200;
  App app(config, std::make_unique<SystemClock>());
  HV_CHECK_EQ(app.run(options), 0);

  const std::vector<std::string> beats = hub.heartbeats();
  HV_CHECK_FALSE(beats.empty());
  if (!beats.empty()) {
    const nlohmann::json body = nlohmann::json::parse(beats.front());
    HV_CHECK_STR_EQ(body.at("device_id").get<std::string>(), "dev-app");
    const std::string boot = body.at("boot_id").get<std::string>();
    HV_CHECK_EQ(boot.size(), size_t{8});
    HV_CHECK(is_hex8(boot));
  }
}
