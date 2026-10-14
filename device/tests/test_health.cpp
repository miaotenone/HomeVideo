// Tests for HealthMonitor and ArmController.
//
// The two rules most likely to be got wrong live here as explicit assertions:
//   - a detector that has never run reports nullopt, never 0 or false
//   - a broken detector is "stale" (a gap), while video/recording failure is a
//     false flag; the device sets video_ok=false only when the encoder really
//     stalled, and never before it has produced anything at all.
//
// The arm cases drive a real HubClient against an in-process httplib server
// (same pattern as test_hub_client.cpp) so the wire payload of POST /arm/ack is
// asserted, not a mocked call.

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "harness.h"
#include "httplib.h"
#include "hv/health.h"
#include "json.hpp"

using namespace hv;
namespace fs = std::filesystem;

namespace {

// Unique, cleaned-up scratch directory. One per test so a state file from one
// case can never leak into another.
struct TempDir {
  fs::path path;

  explicit TempDir(const std::string& tag) {
    static std::atomic<int> counter{0};
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path = fs::temp_directory_path() /
           ("hv_health_" + tag + "_" + std::to_string(stamp) + "_" +
            std::to_string(counter.fetch_add(1)));
    std::error_code ec;
    fs::create_directories(path, ec);
  }

  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
};

// Real HTTP server on an ephemeral port; joined in the destructor so a failing
// HV_CHECK still stops the listener and the thread.
class TestServer {
 public:
  explicit TestServer(std::function<void(httplib::Server&)> setup) {
    setup(server_);
    port_ = server_.bind_to_any_port("127.0.0.1");
    thread_ = std::thread([this] { server_.listen_after_bind(); });
    server_.wait_until_ready();
  }

  ~TestServer() {
    server_.stop();
    if (thread_.joinable()) thread_.join();
  }

  TestServer(const TestServer&) = delete;
  TestServer& operator=(const TestServer&) = delete;

  std::string url() const {
    return "http://127.0.0.1:" + std::to_string(port_);
  }

 private:
  httplib::Server server_;
  std::thread thread_;
  int port_ = 0;
};

// Desired arm config plus the acks the server actually received. Handlers run on
// the server thread, so every access is under the mutex.
struct ArmServerState {
  std::mutex mu;
  std::string mode = "away";
  int version = 7;
  std::vector<nlohmann::json> acks;
};

void install_arm_routes(httplib::Server& server, ArmServerState& state) {
  server.Get("/api/v1/arm", [&state](const httplib::Request&,
                                     httplib::Response& res) {
    std::lock_guard<std::mutex> lock(state.mu);
    nlohmann::json body;
    body["mode"] = state.mode;
    body["config_version"] = state.version;
    res.status = 200;
    res.set_content(body.dump(), "application/json");
  });
  server.Post("/api/v1/arm/ack", [&state](const httplib::Request& req,
                                          httplib::Response& res) {
    std::lock_guard<std::mutex> lock(state.mu);
    state.acks.push_back(nlohmann::json::parse(req.body));
    res.status = 200;
    res.set_content("{\"status\":\"ok\",\"effective\":true}",
                    "application/json");
  });
}

size_t ack_count(ArmServerState& state) {
  std::lock_guard<std::mutex> lock(state.mu);
  return state.acks.size();
}

nlohmann::json ack_at(ArmServerState& state, size_t index) {
  std::lock_guard<std::mutex> lock(state.mu);
  return state.acks.at(index);
}

void write_text(const fs::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
}

}  // namespace

HV_TEST(health_nothing_noted_is_nullopt_never_false) {
  HealthMonitor monitor("dev-a", "boot-1");
  FakeClock clock;
  HealthThresholds thresholds;

  // A detector that has never run must be null (unknown to the hub), not 0 —
  // 0 would read as "ran at boot" and never as "broken".
  HV_CHECK_FALSE(monitor.last_detection_ms(EventType::PersonIntrusion).has_value());
  HV_CHECK_FALSE(monitor.last_detection_ms(EventType::FireSuspected).has_value());
  HV_CHECK_FALSE(monitor.last_detection_ms(EventType::SmokeSuspected).has_value());
  HV_CHECK_NE(monitor.last_detection_ms(EventType::PersonIntrusion),
              std::optional<MonotonicMs>(0));

  HV_CHECK_FALSE(monitor.video_ok(clock.monotonic_ms(), thresholds).has_value());
  HV_CHECK_FALSE(monitor.recording_ok(clock.monotonic_ms(), thresholds).has_value());
  HV_CHECK_NE(monitor.video_ok(clock.monotonic_ms(), thresholds),
              std::optional<bool>(false));
  HV_CHECK_NE(monitor.recording_ok(clock.monotonic_ms(), thresholds),
              std::optional<bool>(false));

  // The report must carry the same nullopt; a heartbeated "false" would tell the
  // hub the hardware failed before it ever ran.
  const DeviceReport report =
      monitor.build(clock.monotonic_ms(), thresholds, ArmMode::Unknown, 0, false);
  HV_CHECK_FALSE(report.person_last_detection_ms.has_value());
  HV_CHECK_FALSE(report.fire_last_detection_ms.has_value());
  HV_CHECK_FALSE(report.smoke_last_detection_ms.has_value());
  HV_CHECK_FALSE(report.video_ok.has_value());
  HV_CHECK_FALSE(report.recording_ok.has_value());
  HV_CHECK_NE(report.video_ok, std::optional<bool>(false));
  HV_CHECK_NE(report.recording_ok, std::optional<bool>(false));
  HV_CHECK_FALSE(report.last_frame_ms.has_value());
  HV_CHECK_FALSE(report.last_encode_ms.has_value());
}

HV_TEST(health_video_ok_starts_nullopt_then_flips_stale) {
  HealthMonitor monitor("dev-a", "boot-1");
  FakeClock clock;
  HealthThresholds thresholds;
  thresholds.video_stale_ms = 100;

  // Never false before the encoder has produced its first chunk.
  HV_CHECK_NE(monitor.video_ok(clock.monotonic_ms(), thresholds),
              std::optional<bool>(false));
  clock.advance_ms(50);
  HV_CHECK_FALSE(monitor.video_ok(clock.monotonic_ms(), thresholds).has_value());

  monitor.note_encode(clock.monotonic_ms());
  HV_CHECK_EQ(monitor.video_ok(clock.monotonic_ms(), thresholds).value_or(false), true);

  const DeviceReport report =
      monitor.build(clock.monotonic_ms(), thresholds, ArmMode::Away, 1, true);
  HV_CHECK_EQ(report.last_encode_ms.value_or(-1), clock.monotonic_ms());
  HV_CHECK_EQ(report.video_ok.value_or(false), true);

  // Small steps: ok while the age is <= the threshold, stale once past it.
  for (int step = 1; step <= 10; ++step) {
    clock.advance_ms(10);
    HV_CHECK_EQ(monitor.video_ok(clock.monotonic_ms(), thresholds).value_or(false),
                true);
  }
  clock.advance_ms(10);  // age 110 > 100
  HV_CHECK_TRUE(monitor.video_ok(clock.monotonic_ms(), thresholds).has_value());
  HV_CHECK_EQ(monitor.video_ok(clock.monotonic_ms(), thresholds).value_or(true),
              false);
}

HV_TEST(health_recording_ok_reflects_last_note_then_staleness) {
  HealthMonitor monitor("dev-a", "boot-1");
  FakeClock clock;
  HealthThresholds thresholds;
  thresholds.recording_stale_ms = 100;

  HV_CHECK_NE(monitor.recording_ok(clock.monotonic_ms(), thresholds),
              std::optional<bool>(false));

  monitor.note_recording_ok(false, clock.monotonic_ms());
  HV_CHECK_TRUE(monitor.recording_ok(clock.monotonic_ms(), thresholds).has_value());
  HV_CHECK_EQ(monitor.recording_ok(clock.monotonic_ms(), thresholds).value_or(true),
              false);
  HV_CHECK_EQ(monitor.build(clock.monotonic_ms(), thresholds, ArmMode::Home, 2, false)
                  .recording_ok.value_or(true),
              false);

  monitor.note_recording_ok(true, clock.monotonic_ms());
  HV_CHECK_EQ(monitor.recording_ok(clock.monotonic_ms(), thresholds).value_or(false),
              true);
  HV_CHECK_EQ(monitor.build(clock.monotonic_ms(), thresholds, ArmMode::Home, 2, false)
                  .recording_ok.value_or(false),
              true);

  for (int step = 1; step <= 10; ++step) {
    clock.advance_ms(10);
    HV_CHECK_EQ(monitor.recording_ok(clock.monotonic_ms(), thresholds).value_or(false),
                true);
  }
  clock.advance_ms(10);  // age 110 > 100
  HV_CHECK_EQ(monitor.recording_ok(clock.monotonic_ms(), thresholds).value_or(true),
              false);
}

HV_TEST(health_detection_is_per_label) {
  HealthMonitor monitor("dev-a", "boot-1");
  HealthThresholds thresholds;
  monitor.note_detection(EventType::FireSuspected, 1234);

  HV_CHECK_EQ(monitor.last_detection_ms(EventType::FireSuspected).value_or(-1), 1234);
  HV_CHECK_FALSE(monitor.last_detection_ms(EventType::PersonIntrusion).has_value());
  HV_CHECK_FALSE(monitor.last_detection_ms(EventType::SmokeSuspected).has_value());

  const DeviceReport report = monitor.build(9999, thresholds, ArmMode::Away, 3, true);
  HV_CHECK_EQ(report.fire_last_detection_ms.value_or(-1), 1234);
  HV_CHECK_FALSE(report.person_last_detection_ms.has_value());
  HV_CHECK_FALSE(report.smoke_last_detection_ms.has_value());
}

HV_TEST(health_build_reports_uptime_and_passthrough_fields) {
  HealthMonitor monitor("dev-a", "boot-1");
  HealthThresholds thresholds;
  monitor.note_frame(10);
  monitor.note_storage_free(4096);

  const DeviceReport report =
      monitor.build(777, thresholds, ArmMode::Away, 5, true);
  HV_CHECK_STR_EQ(report.device_id, "dev-a");
  HV_CHECK_STR_EQ(report.boot_id, "boot-1");
  HV_CHECK_EQ(report.uptime_ms.value_or(-1), 777);  // uptime is the now passed in
  HV_CHECK_EQ(report.last_frame_ms.value_or(-1), 10);
  HV_CHECK_EQ(report.storage_free_bytes.value_or(0), uint64_t{4096});
  HV_CHECK_EQ(report.arm_applied.value_or(ArmMode::Unknown), ArmMode::Away);
  HV_CHECK_EQ(report.arm_applied_version.value_or(-1), 5);
  HV_CHECK_TRUE(report.time_trusted);
}

HV_TEST(arm_controller_fresh_defaults_to_unknown) {
  const TempDir dir("fresh");
  HubClient client("http://127.0.0.1:1", "dev-a", 200);
  FakeClock clock;
  ArmController arm(client, clock, dir.path);

  HV_CHECK_EQ(arm.applied_mode(), ArmMode::Unknown);
  HV_CHECK_EQ(arm.applied_version(), 0);
  HV_CHECK_FALSE(arm.should_run_person_detection());
  HV_CHECK_EQ(arm.acks_sent(), uint64_t{0});
}

HV_TEST(arm_poll_applies_new_version_and_acks_payload) {
  const TempDir dir("apply");
  ArmServerState state;
  TestServer server([&](httplib::Server& s) { install_arm_routes(s, state); });
  HubClient client(server.url(), "dev-a", 2000);
  FakeClock clock;
  ArmController arm(client, clock, dir.path);

  HV_CHECK(arm.poll_and_apply().ok());
  HV_CHECK_EQ(arm.applied_mode(), ArmMode::Away);
  HV_CHECK_EQ(arm.applied_version(), 7);
  HV_CHECK_TRUE(arm.should_run_person_detection());
  HV_CHECK_EQ(arm.acks_sent(), uint64_t{1});

  HV_CHECK_EQ(ack_count(state), size_t{1});
  const nlohmann::json ack = ack_at(state, 0);
  HV_CHECK_STR_EQ(ack.at("device_id").get<std::string>(), "dev-a");
  HV_CHECK_STR_EQ(ack.at("applied_mode").get<std::string>(), "away");
  HV_CHECK_EQ(ack.at("applied_version").get<int>(), 7);
}

HV_TEST(arm_poll_same_version_sends_no_second_ack) {
  const TempDir dir("same");
  ArmServerState state;
  TestServer server([&](httplib::Server& s) { install_arm_routes(s, state); });
  HubClient client(server.url(), "dev-a", 2000);
  FakeClock clock;
  ArmController arm(client, clock, dir.path);

  HV_CHECK(arm.poll_and_apply().ok());
  HV_CHECK(arm.poll_and_apply().ok());
  HV_CHECK_EQ(ack_count(state), size_t{1});
  HV_CHECK_EQ(arm.acks_sent(), uint64_t{1});
  HV_CHECK_EQ(arm.applied_version(), 7);
}

HV_TEST(arm_poll_higher_version_applies_and_acks_again) {
  const TempDir dir("bump");
  ArmServerState state;
  TestServer server([&](httplib::Server& s) { install_arm_routes(s, state); });
  HubClient client(server.url(), "dev-a", 2000);
  FakeClock clock;
  ArmController arm(client, clock, dir.path);

  HV_CHECK(arm.poll_and_apply().ok());
  {
    std::lock_guard<std::mutex> lock(state.mu);
    state.version = 8;
    state.mode = "home";
  }
  HV_CHECK(arm.poll_and_apply().ok());

  HV_CHECK_EQ(arm.applied_mode(), ArmMode::Home);
  HV_CHECK_EQ(arm.applied_version(), 8);
  HV_CHECK_FALSE(arm.should_run_person_detection());
  HV_CHECK_EQ(ack_count(state), size_t{2});
  HV_CHECK_EQ(arm.acks_sent(), uint64_t{2});
  HV_CHECK_STR_EQ(ack_at(state, 1).at("applied_mode").get<std::string>(), "home");
}

// An older version is not a downgrade to unarmed: the hub's "unknown, 0" is the
// never-initialised state, and a device already enforcing away/7 keeps doing so.
// hub/README.md "布防与检测语义": 未初始化（unknown、版本 0）绝不显示为已布防；
// and a lower config_version is 落后 / 过期 and 不生效, so it is not applied.
HV_TEST(arm_poll_unknown_zero_does_not_downgrade_applied_mode) {
  const TempDir dir("unknown");
  ArmServerState state;
  TestServer server([&](httplib::Server& s) { install_arm_routes(s, state); });
  HubClient client(server.url(), "dev-a", 2000);
  FakeClock clock;
  ArmController arm(client, clock, dir.path);

  HV_CHECK(arm.poll_and_apply().ok());
  HV_CHECK_EQ(arm.applied_version(), 7);

  {
    std::lock_guard<std::mutex> lock(state.mu);
    state.mode = "unknown";
    state.version = 0;
  }
  HV_CHECK(arm.poll_and_apply().ok());

  HV_CHECK_EQ(arm.applied_mode(), ArmMode::Away);
  HV_CHECK_EQ(arm.applied_version(), 7);
  HV_CHECK_TRUE(arm.should_run_person_detection());
  HV_CHECK_EQ(ack_count(state), size_t{1});  // no new ack
  HV_CHECK_EQ(arm.acks_sent(), uint64_t{1});
}

HV_TEST(arm_poll_network_failure_keeps_applied_and_acks_nothing) {
  const TempDir dir("netfail");
  // Seed a confirmed configuration so "unchanged from before the call" is
  // meaningful: the device is enforcing away/7 and the poll fails.
  write_text(dir.path / "device_state.json",
             "{\"applied_mode\":\"away\",\"applied_version\":7}");

  // Port 1 has no listener, so the connection is refused immediately.
  HubClient client("http://127.0.0.1:1", "dev-a", 300);
  FakeClock clock;
  ArmController arm(client, clock, dir.path);

  HV_CHECK(arm.load_persisted().ok());
  HV_CHECK_EQ(arm.applied_mode(), ArmMode::Away);
  HV_CHECK_EQ(arm.applied_version(), 7);

  HV_CHECK_FALSE(arm.poll_and_apply().ok());
  HV_CHECK_EQ(arm.applied_mode(), ArmMode::Away);
  HV_CHECK_EQ(arm.applied_version(), 7);
  HV_CHECK_EQ(arm.acks_sent(), uint64_t{0});  // no ack was sent
}

HV_TEST(arm_persist_round_trips_through_a_new_controller) {
  const TempDir dir("persist");
  // A nested, not-yet-existing directory proves persist() creates it.
  const fs::path state_dir = dir.path / "state";
  ArmServerState state;
  TestServer server([&](httplib::Server& s) { install_arm_routes(s, state); });
  HubClient client(server.url(), "dev-a", 2000);
  FakeClock clock;

  ArmController first(client, clock, state_dir);
  HV_CHECK(first.poll_and_apply().ok());
  HV_CHECK_EQ(first.applied_version(), 7);

  ArmController second(client, clock, state_dir);
  HV_CHECK(second.load_persisted().ok());
  HV_CHECK_EQ(second.applied_mode(), ArmMode::Away);
  HV_CHECK_EQ(second.applied_version(), 7);
  HV_CHECK_TRUE(second.should_run_person_detection());
}

HV_TEST(arm_load_persisted_missing_file_is_ok_defaults_and_garbage_is_error) {
  const TempDir dir("load");
  HubClient client("http://127.0.0.1:1", "dev-a", 200);
  FakeClock clock;

  {
    ArmController fresh(client, clock, dir.path / "absent");
    HV_CHECK(fresh.load_persisted().ok());
    HV_CHECK_EQ(fresh.applied_mode(), ArmMode::Unknown);
    HV_CHECK_EQ(fresh.applied_version(), 0);
  }

  {
    write_text(dir.path / "device_state.json", "not json at all");
    ArmController broken(client, clock, dir.path);
    const Result loaded = broken.load_persisted();
    HV_CHECK_FALSE(loaded.ok());
    // Defaults stand after a malformed read, no exception and no partial state.
    HV_CHECK_EQ(broken.applied_mode(), ArmMode::Unknown);
    HV_CHECK_EQ(broken.applied_version(), 0);
  }

  {
    // Parseable JSON but the wrong shape is still malformed.
    write_text(dir.path / "device_state.json", "{\"applied_mode\":\"away\"}");
    ArmController broken(client, clock, dir.path);
    HV_CHECK_FALSE(broken.load_persisted().ok());
    HV_CHECK_EQ(broken.applied_mode(), ArmMode::Unknown);
    HV_CHECK_EQ(broken.applied_version(), 0);
  }
}
