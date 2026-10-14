#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <string>
#include <thread>

#include "harness.h"
#include "httplib.h"
#include "hv/hub_client.h"
#include "json.hpp"

using namespace hv;
namespace fs = std::filesystem;

namespace {

// 1994-11-06T08:49:37Z, computed with
// python3 -c "import calendar,time;print(calendar.timegm(time.strptime(
//   'Sun, 06 Nov 1994 08:49:37 GMT','%a, %d %b %Y %H:%M:%S GMT')))"
constexpr int64_t kKnownUnix = 784111777;

// Starts a real httplib server on an ephemeral port and joins it in the
// destructor, so a failing HV_CHECK still stops the listener and the thread.
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

fs::path write_file(const std::string& name, const std::string& bytes) {
  const fs::path path = fs::temp_directory_path() / ("hv_hub_" + name);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << bytes;
  out.close();
  return path;
}

std::set<std::string> key_set(const nlohmann::json& object) {
  std::set<std::string> keys;
  for (auto it = object.begin(); it != object.end(); ++it) keys.insert(it.key());
  return keys;
}

}  // namespace

HV_TEST(hub_client_heartbeat_body_matches_device_report_wire_fields) {
  std::string captured;
  TestServer server([&](httplib::Server& s) {
    s.Post("/api/v1/device/heartbeat",
           [&](const httplib::Request& req, httplib::Response& res) {
             captured = req.body;
             res.status = 200;
             res.set_content("{\"device_id\":\"dev-a\",\"status\":\"ok\"}",
                             "application/json");
           });
  });

  HubClient client(server.url(), "dev-a", 2000);
  DeviceReport report;
  report.device_id = "dev-a";
  report.boot_id = "boot-1";
  report.uptime_ms = 1000;
  report.last_frame_ms = 900;
  report.last_encode_ms = 800;
  report.person_last_detection_ms = std::nullopt;
  report.fire_last_detection_ms = 700;
  report.smoke_last_detection_ms = std::nullopt;
  report.video_ok = true;
  report.recording_ok = false;
  report.storage_free_bytes = 123456;
  report.arm_applied = ArmMode::Away;
  report.arm_applied_version = 4;
  report.time_trusted = true;

  const HeartbeatResult result = client.heartbeat(report);
  HV_CHECK(result.result.ok());

  const std::set<std::string> expected = {
      "device_id",              "boot_id",
      "uptime_ms",              "last_frame_ms",
      "last_encode_ms",         "person_last_detection_ms",
      "fire_last_detection_ms", "smoke_last_detection_ms",
      "video_ok",               "recording_ok",
      "storage_free_bytes",     "arm_applied",
      "arm_applied_version",    "time_trusted"};
  const nlohmann::json body = nlohmann::json::parse(captured);
  const std::set<std::string> actual = key_set(body);
  HV_CHECK_EQ(actual.size(), expected.size());
  HV_CHECK(actual == expected);
}

HV_TEST(hub_client_never_fired_detector_serialises_as_null) {
  DeviceReport report;
  report.device_id = "dev-a";
  report.boot_id = "boot-1";
  report.person_last_detection_ms = std::nullopt;
  report.fire_last_detection_ms = std::nullopt;
  report.smoke_last_detection_ms = std::nullopt;

  const std::string body = device_report_to_json(report);
  HV_CHECK(body.find("\"person_last_detection_ms\":null") != std::string::npos);
  HV_CHECK(body.find("\"fire_last_detection_ms\":null") != std::string::npos);
  HV_CHECK(body.find("\"smoke_last_detection_ms\":null") != std::string::npos);
  HV_CHECK(body.find("\"person_last_detection_ms\":0") == std::string::npos);
}

HV_TEST(hub_client_heartbeat_reads_server_date_header) {
  TestServer server([](httplib::Server& s) {
    s.Post("/api/v1/device/heartbeat",
           [](const httplib::Request&, httplib::Response& res) {
             res.status = 200;
             res.set_header("Date", "Sun, 06 Nov 1994 08:49:37 GMT");
             res.set_content("{\"device_id\":\"dev-a\",\"status\":\"ok\"}",
                             "application/json");
           });
  });

  HubClient client(server.url(), "dev-a", 2000);
  DeviceReport report;
  report.device_id = "dev-a";

  const HeartbeatResult result = client.heartbeat(report);
  HV_CHECK(result.result.ok());
  HV_CHECK(result.server_unix.has_value());
  HV_CHECK_EQ(result.server_unix.value_or(0), kKnownUnix);
  HV_CHECK_EQ(client.last_server_unix().value_or(0), kKnownUnix);
}

HV_TEST(hub_client_heartbeat_without_date_header_reports_no_time) {
  TestServer server([](httplib::Server& s) {
    s.Post("/api/v1/device/heartbeat",
           [](const httplib::Request&, httplib::Response& res) {
             res.status = 200;
             res.set_content("{\"device_id\":\"dev-a\",\"status\":\"ok\"}",
                             "application/json");
           });
  });

  HubClient client(server.url(), "dev-a", 2000);
  DeviceReport report;
  report.device_id = "dev-a";

  const HeartbeatResult result = client.heartbeat(report);
  HV_CHECK(result.result.ok());
  HV_CHECK_FALSE(result.server_unix.has_value());
  HV_CHECK_FALSE(client.last_server_unix().has_value());
}

HV_TEST(hub_client_get_arm_reads_desired_mode_and_version) {
  TestServer server([](httplib::Server& s) {
    s.Get("/api/v1/arm", [](const httplib::Request&, httplib::Response& res) {
      res.status = 200;
      res.set_content("{\"mode\":\"away\",\"config_version\":7}",
                      "application/json");
    });
  });

  HubClient client(server.url(), "dev-a", 2000);
  ArmMode mode = ArmMode::Home;
  int version = -1;
  const Result result = client.get_arm(mode, version);
  HV_CHECK(result.ok());
  HV_CHECK_EQ(mode, ArmMode::Away);
  HV_CHECK_EQ(version, 7);
}

HV_TEST(hub_client_get_arm_accepts_unknown_zero) {
  TestServer server([](httplib::Server& s) {
    s.Get("/api/v1/arm", [](const httplib::Request&, httplib::Response& res) {
      res.status = 200;
      res.set_content("{\"mode\":\"unknown\",\"config_version\":0}",
                      "application/json");
    });
  });

  HubClient client(server.url(), "dev-a", 2000);
  ArmMode mode = ArmMode::Away;
  int version = 9;
  const Result result = client.get_arm(mode, version);
  HV_CHECK(result.ok());
  HV_CHECK_EQ(mode, ArmMode::Unknown);
  HV_CHECK_EQ(version, 0);
}

HV_TEST(hub_client_get_arm_rejects_malformed_body) {
  TestServer server([](httplib::Server& s) {
    s.Get("/api/v1/arm", [](const httplib::Request&, httplib::Response& res) {
      res.status = 200;
      res.set_content("not json", "text/plain");
    });
  });

  HubClient client(server.url(), "dev-a", 2000);
  ArmMode mode = ArmMode::Home;
  int version = 0;
  const Result result = client.get_arm(mode, version);
  HV_CHECK_FALSE(result.ok());
  HV_CHECK_FALSE(client.last_error().empty());
}

HV_TEST(hub_client_create_event_distinguishes_created_from_existing) {
  int status = 201;
  std::string body = "{\"event_id\":\"evt-1\",\"created\":true}";
  TestServer server([&](httplib::Server& s) {
    s.Post("/api/v1/events", [&](const httplib::Request&, httplib::Response& res) {
      res.status = status;
      res.set_content(body, "application/json");
    });
  });

  HubClient client(server.url(), "dev-a", 2000);
  EventMetadata metadata;
  metadata.event_id = "evt-1";
  metadata.device_id = "dev-a";
  metadata.event_type = "person_intrusion";
  metadata.model_version = "model-v1";
  metadata.arm_mode = "away";

  EventUpsertResult result = client.create_event(metadata);
  HV_CHECK(result.result.ok());
  HV_CHECK_TRUE(result.created);

  status = 200;
  body = "{\"event_id\":\"evt-1\",\"created\":false}";
  result = client.create_event(metadata);
  HV_CHECK(result.result.ok());
  HV_CHECK_FALSE(result.created);

  status = 422;
  body = "{\"detail\":\"invalid event_id\"}";
  result = client.create_event(metadata);
  HV_CHECK_FALSE(result.result.ok());
  HV_CHECK_EQ(result.result.code(), Code::Invalid);
  HV_CHECK(result.result.message().find("invalid event_id") != std::string::npos);
}

HV_TEST(hub_client_upload_snapshot_maps_hub_outcomes) {
  int status = 201;
  std::string body = "{\"event_id\":\"evt-1\",\"status\":\"stored\"}";
  TestServer server([&](httplib::Server& s) {
    s.Post("/api/v1/events/:event_id/snapshot",
           [&](const httplib::Request&, httplib::Response& res) {
             res.status = status;
             res.set_content(body, "application/json");
           });
  });

  const fs::path file = write_file("snapshot.jpg", "jpeg-bytes");
  HubClient client(server.url(), "dev-a", 2000);

  HV_CHECK_EQ(client.upload_snapshot("evt-1", file, "image/jpeg"),
              UploadOutcome::Ok);

  status = 200;
  body = "{\"event_id\":\"evt-1\",\"status\":\"duplicate\"}";
  HV_CHECK_EQ(client.upload_snapshot("evt-1", file, "image/jpeg"),
              UploadOutcome::Duplicate);

  status = 404;
  body = "{\"detail\":\"event not found\"}";
  HV_CHECK_EQ(client.upload_snapshot("evt-1", file, "image/jpeg"),
              UploadOutcome::EventMissing);

  status = 507;
  body = "{\"detail\":\"insufficient storage\"}";
  HV_CHECK_EQ(client.upload_snapshot("evt-1", file, "image/jpeg"),
              UploadOutcome::InsufficientStorage);
}

HV_TEST(hub_client_upload_segment_sends_checksum_and_maps_conflicts) {
  int status = 201;
  std::string seen_checksum;
  TestServer server([&](httplib::Server& s) {
    s.Put("/api/v1/events/:event_id/segments/:seq",
          [&](const httplib::Request& req, httplib::Response& res) {
            seen_checksum = req.get_header_value("X-Segment-Sha256");
            res.status = status;
            res.set_content("{\"status\":\"stored\"}", "application/json");
          });
  });

  const fs::path file = write_file("segment.bin", "segment-bytes");
  HubClient client(server.url(), "dev-a", 2000);
  const std::string sha = "abc123def456";

  HV_CHECK_EQ(client.upload_segment("evt-1", 0, file, sha), UploadOutcome::Ok);
  HV_CHECK_STR_EQ(seen_checksum, sha);

  status = 200;
  HV_CHECK_EQ(client.upload_segment("evt-1", 0, file, sha),
              UploadOutcome::Duplicate);

  status = 409;
  HV_CHECK_EQ(client.upload_segment("evt-1", 0, file, sha),
              UploadOutcome::Conflict);

  status = 400;
  HV_CHECK_EQ(client.upload_segment("evt-1", 0, file, sha),
              UploadOutcome::ChecksumMismatch);

  // The declared checksum accompanies every attempt, not just the first.
  HV_CHECK_STR_EQ(seen_checksum, sha);
}

HV_TEST(hub_client_ack_arm_sends_applied_mode_and_version) {
  std::string captured;
  TestServer server([&](httplib::Server& s) {
    s.Post("/api/v1/arm/ack",
           [&](const httplib::Request& req, httplib::Response& res) {
             captured = req.body;
             res.status = 200;
             res.set_content("{\"status\":\"ok\",\"effective\":true}",
                             "application/json");
           });
  });

  HubClient client(server.url(), "dev-a", 2000);
  const Result result = client.ack_arm(ArmMode::Away, 7);
  HV_CHECK(result.ok());

  const nlohmann::json body = nlohmann::json::parse(captured);
  HV_CHECK_STR_EQ(body.at("device_id").get<std::string>(), "dev-a");
  HV_CHECK_STR_EQ(body.at("applied_mode").get<std::string>(), "away");
  HV_CHECK_EQ(body.at("applied_version").get<int>(), 7);
}

HV_TEST(hub_client_network_failure_never_throws) {
  // Port 1 has no listener, so the connection is refused immediately.
  HubClient client("http://127.0.0.1:1", "dev-a", 500);
  const fs::path file = write_file("refused.bin", "x");

  HV_CHECK_EQ(client.upload_snapshot("evt-1", file, "image/jpeg"),
              UploadOutcome::NetworkError);
  HV_CHECK_EQ(client.upload_segment("evt-1", 0, file, "deadbeef"),
              UploadOutcome::NetworkError);

  ArmMode mode = ArmMode::Home;
  int version = 0;
  HV_CHECK_FALSE(client.get_arm(mode, version).ok());
  HV_CHECK_FALSE(client.ack_arm(ArmMode::Away, 1).ok());

  DeviceReport report;
  report.device_id = "dev-a";
  HV_CHECK_FALSE(client.heartbeat(report).result.ok());

  EventMetadata metadata;
  metadata.event_id = "evt-1";
  metadata.device_id = "dev-a";
  metadata.event_type = "person_intrusion";
  HV_CHECK_FALSE(client.create_event(metadata).result.ok());
  HV_CHECK_FALSE(client.close_event("evt-1", EventClosePayload{}).ok());
  HV_CHECK_FALSE(client.cleanup().ok());
  HV_CHECK_FALSE(client.last_error().empty());
}

HV_TEST(hub_client_parse_http_date_handles_rfc7231_forms) {
  const auto imf = parse_http_date("Sun, 06 Nov 1994 08:49:37 GMT");
  HV_CHECK(imf.has_value());
  HV_CHECK_EQ(imf.value_or(0), kKnownUnix);

  const auto rfc850 = parse_http_date("Sunday, 06-Nov-94 08:49:37 GMT");
  HV_CHECK(rfc850.has_value());
  HV_CHECK_EQ(rfc850.value_or(0), kKnownUnix);

  HV_CHECK_FALSE(parse_http_date("").has_value());
  HV_CHECK_FALSE(parse_http_date("garbage").has_value());
  HV_CHECK_FALSE(parse_http_date("Sun, 06 Nov 1994 08:49:37").has_value());
  HV_CHECK_FALSE(parse_http_date("Sun, 31 Feb 1994 08:49:37 GMT").has_value());
}
