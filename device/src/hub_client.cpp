#include "hv/hub_client.h"

// httplib.h and json.hpp live only in this translation unit: the header is
// included by the uploader/health modules, which must not pay for either.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>

#include "httplib.h"
#include "json.hpp"
#include "json_util.h"

namespace hv {

namespace {

using nlohmann::json;

constexpr const char* kApiPrefix = "/api/v1";

// nlohmann::json has no implicit optional conversion, so an absent value has to
// be written as an explicit null. This is load-bearing: a detector that never
// fired must reach the hub as null, never as 0.
template <class T>
void set_optional(json& out, const char* key, const std::optional<T>& value) {
  if (value.has_value()) {
    out[key] = *value;
  } else {
    out[key] = nullptr;
  }
}

const char* const kMonthNames[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

std::optional<int> month_from_name(const char* name) {
  for (int i = 0; i < 12; ++i) {
    if (std::strcmp(name, kMonthNames[i]) == 0) return i;
  }
  return std::nullopt;
}

// timegm normalises impossible dates (Feb 30 -> Mar 2) instead of failing, so
// round-trip through gmtime_r and reject anything that moved.
std::optional<int64_t> make_unix(int year, int month, int day, int hour,
                                 int minute, int second) {
  if (month < 0 || month > 11 || day < 1 || day > 31 || hour < 0 ||
      hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60) {
    return std::nullopt;
  }
  std::tm tm{};
  tm.tm_year = year - 1900;
  tm.tm_mon = month;
  tm.tm_mday = day;
  tm.tm_hour = hour;
  tm.tm_min = minute;
  tm.tm_sec = second;

  // timegm normalises tm in place, so compare against the original arguments.
  const std::time_t seconds = timegm(&tm);
  std::tm check{};
  if (gmtime_r(&seconds, &check) == nullptr) return std::nullopt;
  if (check.tm_year != year - 1900 || check.tm_mon != month ||
      check.tm_mday != day || check.tm_hour != hour ||
      check.tm_min != minute || check.tm_sec != second) {
    return std::nullopt;
  }
  return static_cast<int64_t>(seconds);
}

bool read_file(const std::filesystem::path& path, std::string& out) {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) return false;
  out.assign(std::istreambuf_iterator<char>(input),
             std::istreambuf_iterator<char>());
  return !input.bad();
}

UploadOutcome classify_upload_status(int status) {
  switch (status) {
    case 201:
      return UploadOutcome::Ok;
    case 200:
      return UploadOutcome::Duplicate;
    case 400:
      return UploadOutcome::ChecksumMismatch;
    case 404:
      return UploadOutcome::EventMissing;
    case 409:
      return UploadOutcome::Conflict;
    case 422:
      return UploadOutcome::Invalid;
    case 507:
      return UploadOutcome::InsufficientStorage;
    default:
      return status >= 500 ? UploadOutcome::ServerError
                           : UploadOutcome::Invalid;
  }
}

}  // namespace

std::optional<int64_t> parse_http_date(const std::string& value) {
  if (value.empty()) return std::nullopt;

  // RFC 7231 IMF-fixdate: "Sun, 06 Nov 1994 08:49:37 GMT".
  {
    char weekday[16] = {0};
    char month[16] = {0};
    int day = 0;
    int year = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int used = 0;
    const int matched = std::sscanf(
        value.c_str(),
        "%15[A-Za-z], %2d %15[A-Za-z] %4d %2d:%2d:%2d GMT%n", weekday, &day,
        month, &year, &hour, &minute, &second, &used);
    if (matched == 7 && used == static_cast<int>(value.size())) {
      const auto parsed_month = month_from_name(month);
      if (parsed_month.has_value()) {
        return make_unix(year, *parsed_month, day, hour, minute, second);
      }
    }
  }

  // Obsolete RFC 850 form: "Sunday, 06-Nov-94 08:49:37 GMT". RFC 7231 still
  // requires recipients to accept it.
  {
    char weekday[16] = {0};
    char month[16] = {0};
    int day = 0;
    int short_year = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int used = 0;
    const int matched = std::sscanf(
        value.c_str(), "%15[A-Za-z], %2d-%15[A-Za-z]-%2d %2d:%2d:%2d GMT%n",
        weekday, &day, month, &short_year, &hour, &minute, &second, &used);
    if (matched == 7 && used == static_cast<int>(value.size())) {
      const auto parsed_month = month_from_name(month);
      if (parsed_month.has_value()) {
        // Two-digit year window fixed by RFC 850/HTTP: 70..99 -> 19xx.
        const int year =
            short_year < 70 ? 2000 + short_year : 1900 + short_year;
        return make_unix(year, *parsed_month, day, hour, minute, second);
      }
    }
  }

  return std::nullopt;
}

std::string device_report_to_json(const DeviceReport& report) {
  json out;
  out["device_id"] = report.device_id;
  out["boot_id"] = report.boot_id;
  set_optional(out, "uptime_ms", report.uptime_ms);
  set_optional(out, "last_frame_ms", report.last_frame_ms);
  set_optional(out, "last_encode_ms", report.last_encode_ms);
  set_optional(out, "person_last_detection_ms", report.person_last_detection_ms);
  set_optional(out, "fire_last_detection_ms", report.fire_last_detection_ms);
  set_optional(out, "smoke_last_detection_ms", report.smoke_last_detection_ms);
  set_optional(out, "video_ok", report.video_ok);
  set_optional(out, "recording_ok", report.recording_ok);
  set_optional(out, "storage_free_bytes", report.storage_free_bytes);
  if (report.arm_applied.has_value()) {
    out["arm_applied"] = to_wire(*report.arm_applied);
  } else {
    out["arm_applied"] = nullptr;
  }
  set_optional(out, "arm_applied_version", report.arm_applied_version);
  out["time_trusted"] = report.time_trusted;
  return out.dump();
}

std::string event_metadata_to_json(const EventMetadata& metadata) {
  json out;
  out["event_id"] = metadata.event_id;
  out["device_id"] = metadata.device_id;
  out["event_type"] = metadata.event_type;
  set_optional(out, "started_at", metadata.started_at);
  out["time_trusted"] = metadata.time_trusted;
  set_optional(out, "device_boot_id", metadata.device_boot_id);
  set_optional(out, "device_started_ms", metadata.device_started_ms);
  out["model_version"] = metadata.model_version;
  out["config_version"] = metadata.config_version;
  out["arm_mode"] = metadata.arm_mode;
  out["detections"] = detections_to_json(metadata.detections);
  return out.dump();
}

std::string event_close_to_json(const EventClosePayload& payload) {
  json out;
  set_optional(out, "ended_at", payload.ended_at);
  set_optional(out, "duration_ms", payload.duration_ms);
  out["time_trusted"] = payload.time_trusted;
  return out.dump();
}

const char* to_string(UploadOutcome outcome) {
  switch (outcome) {
    case UploadOutcome::Ok:
      return "Ok";
    case UploadOutcome::Duplicate:
      return "Duplicate";
    case UploadOutcome::Conflict:
      return "Conflict";
    case UploadOutcome::EventMissing:
      return "EventMissing";
    case UploadOutcome::ChecksumMismatch:
      return "ChecksumMismatch";
    case UploadOutcome::InsufficientStorage:
      return "InsufficientStorage";
    case UploadOutcome::Invalid:
      return "Invalid";
    case UploadOutcome::NetworkError:
      return "NetworkError";
    case UploadOutcome::ServerError:
      return "ServerError";
  }
  return "Unknown";
}

struct HubClient::Impl {
  Impl(std::string url, std::string id, int timeout_ms)
      : base_url(std::move(url)), device_id(std::move(id)) {
    if (base_url.empty()) {
      mark_unusable("hub base_url is empty");
      return;
    }
    try {
      client = std::make_unique<httplib::Client>(base_url);
      if (!client->is_valid()) {
        mark_unusable("hub base_url could not be parsed: " + base_url);
        return;
      }
      const auto timeout =
          std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 1);
      client->set_connection_timeout(timeout);
      client->set_read_timeout(timeout);
      client->set_write_timeout(timeout);
    } catch (const std::exception& error) {
      // Unsupported scheme (e.g. https without OpenSSL) throws; the device
      // must keep running, so record why and serve the error state forever.
      mark_unusable(std::string("hub base_url could not be parsed: ") +
                    error.what());
    } catch (...) {
      mark_unusable("hub base_url could not be parsed: " + base_url);
    }
  }

  bool ready() const { return client != nullptr; }

  void mark_unusable(const std::string& reason) {
    client.reset();
    last_error = reason;
  }

  Result make_error(Code code, const std::string& message) {
    last_error = message;
    return Result::error(code, message);
  }

  Result transport_error(const httplib::Result& result) {
    if (result.error() == httplib::Error::ConnectionTimeout) {
      return make_error(Code::Timeout, "hub request timed out");
    }
    return make_error(Code::Protocol, std::string("hub connection failed: ") +
                                          httplib::to_string(result.error()));
  }

  UploadOutcome transport_upload_error(const httplib::Result& result) {
    last_error = std::string("hub connection failed: ") +
                 httplib::to_string(result.error());
    return UploadOutcome::NetworkError;
  }

  Result unavailable() {
    if (last_error.empty()) last_error = "hub client is unavailable";
    return Result::error(Code::Protocol, last_error);
  }

  UploadOutcome unavailable_upload() {
    if (last_error.empty()) last_error = "hub client is unavailable";
    return UploadOutcome::NetworkError;
  }

  void note_date(const httplib::Response& response) {
    last_server_unix = parse_http_date(response.get_header_value("Date"));
  }

  std::unique_ptr<httplib::Client> client;
  std::string base_url;
  std::string device_id;
  std::string last_error;
  std::optional<int64_t> last_server_unix;
};

HubClient::HubClient(std::string base_url, std::string device_id,
                     int timeout_ms)
    : impl_(std::make_shared<Impl>(std::move(base_url), std::move(device_id),
                                   timeout_ms)) {}

HeartbeatResult HubClient::heartbeat(const DeviceReport& report) {
  HeartbeatResult out;
  if (!impl_->ready()) {
    out.result = impl_->unavailable();
    return out;
  }

  httplib::Result response;
  try {
    response = impl_->client->Post(std::string(kApiPrefix) + "/device/heartbeat",
                                   device_report_to_json(report),
                                   "application/json");
  } catch (...) {
    out.result = impl_->make_error(Code::Protocol, "hub heartbeat threw");
    return out;
  }
  if (!response) {
    out.result = impl_->transport_error(response);
    return out;
  }

  impl_->note_date(*response);
  out.server_unix = impl_->last_server_unix;
  if (response->status != 200) {
    out.result = impl_->make_error(
        Code::Protocol,
        "hub heartbeat returned " + std::to_string(response->status));
    return out;
  }
  out.result = Result::success();
  return out;
}

Result HubClient::get_arm(ArmMode& mode, int& config_version) {
  if (!impl_->ready()) return impl_->unavailable();

  httplib::Result response;
  try {
    response = impl_->client->Get(std::string(kApiPrefix) + "/arm");
  } catch (...) {
    return impl_->make_error(Code::Protocol, "hub get_arm threw");
  }
  if (!response) return impl_->transport_error(response);

  impl_->note_date(*response);
  if (response->status != 200) {
    return impl_->make_error(
        Code::Protocol,
        "hub get_arm returned " + std::to_string(response->status));
  }

  json doc;
  try {
    doc = json::parse(response->body);
  } catch (...) {
    return impl_->make_error(Code::Protocol, "hub arm response is not JSON");
  }
  if (!doc.is_object()) {
    return impl_->make_error(Code::Protocol, "hub arm response is not an object");
  }
  const auto mode_it = doc.find("mode");
  if (mode_it == doc.end() || !mode_it->is_string()) {
    return impl_->make_error(Code::Protocol, "hub arm response has no mode");
  }
  const auto parsed = arm_mode_from_wire(mode_it->get<std::string>());
  if (!parsed.has_value()) {
    return impl_->make_error(
        Code::Protocol,
        "hub arm response has an unknown mode: " + mode_it->get<std::string>());
  }
  const auto version_it = doc.find("config_version");
  if (version_it == doc.end() || !version_it->is_number_integer()) {
    return impl_->make_error(Code::Protocol,
                             "hub arm response has no config_version");
  }

  mode = *parsed;
  config_version = version_it->get<int>();
  return Result::success();
}

Result HubClient::ack_arm(ArmMode applied_mode, int applied_version) {
  if (!impl_->ready()) return impl_->unavailable();

  json body;
  body["device_id"] = impl_->device_id;
  body["applied_mode"] = to_wire(applied_mode);
  body["applied_version"] = applied_version;

  httplib::Result response;
  try {
    response = impl_->client->Post(std::string(kApiPrefix) + "/arm/ack",
                                   body.dump(), "application/json");
  } catch (...) {
    return impl_->make_error(Code::Protocol, "hub ack_arm threw");
  }
  if (!response) return impl_->transport_error(response);

  impl_->note_date(*response);
  if (response->status == 200) return Result::success();
  if (response->status == 422) {
    return impl_->make_error(Code::Invalid, response->body);
  }
  return impl_->make_error(
      Code::Protocol,
      "hub ack_arm returned " + std::to_string(response->status));
}

EventUpsertResult HubClient::create_event(const EventMetadata& metadata) {
  EventUpsertResult out;
  if (!impl_->ready()) {
    out.result = impl_->unavailable();
    return out;
  }

  httplib::Result response;
  try {
    response = impl_->client->Post(std::string(kApiPrefix) + "/events",
                                   event_metadata_to_json(metadata),
                                   "application/json");
  } catch (...) {
    out.result = impl_->make_error(Code::Protocol, "hub create_event threw");
    return out;
  }
  if (!response) {
    out.result = impl_->transport_error(response);
    return out;
  }

  impl_->note_date(*response);
  if (response->status == 201) {
    out.result = Result::success();
    out.created = true;
    return out;
  }
  if (response->status == 200) {
    out.result = Result::success();
    out.created = false;
    return out;
  }
  if (response->status == 422) {
    out.result = impl_->make_error(Code::Invalid, response->body);
    return out;
  }
  out.result = impl_->make_error(
      Code::Protocol,
      "hub create_event returned " + std::to_string(response->status));
  return out;
}

UploadOutcome HubClient::upload_snapshot(const std::string& event_id,
                                         const std::filesystem::path& path,
                                         const std::string& content_type) {
  if (!impl_->ready()) return impl_->unavailable_upload();

  std::string body;
  if (!read_file(path, body)) {
    impl_->last_error = "cannot read snapshot file: " + path.string();
    return UploadOutcome::Invalid;
  }

  httplib::Result response;
  try {
    response = impl_->client->Post(
        std::string(kApiPrefix) + "/events/" + event_id + "/snapshot", body,
        content_type.empty() ? "application/octet-stream" : content_type);
  } catch (...) {
    impl_->last_error = "hub upload_snapshot threw";
    return UploadOutcome::NetworkError;
  }
  if (!response) return impl_->transport_upload_error(response);

  impl_->note_date(*response);
  const UploadOutcome outcome = classify_upload_status(response->status);
  if (outcome != UploadOutcome::Ok && outcome != UploadOutcome::Duplicate) {
    impl_->last_error = "hub upload_snapshot returned " +
                        std::to_string(response->status);
  }
  return outcome;
}

UploadOutcome HubClient::upload_segment(const std::string& event_id, int seq,
                                        const std::filesystem::path& path,
                                        const std::string& sha256_hex) {
  if (!impl_->ready()) return impl_->unavailable_upload();

  std::string body;
  if (!read_file(path, body)) {
    impl_->last_error = "cannot read segment file: " + path.string();
    return UploadOutcome::Invalid;
  }

  // The hub validates this exact header name (hub/homevideo_hub/api.py:
  // request.headers.get("x-segment-sha256")).
  httplib::Headers headers;
  headers.emplace("X-Segment-Sha256", sha256_hex);

  httplib::Result response;
  try {
    response = impl_->client->Put(
        std::string(kApiPrefix) + "/events/" + event_id + "/segments/" +
            std::to_string(seq),
        headers, body, "application/octet-stream");
  } catch (...) {
    impl_->last_error = "hub upload_segment threw";
    return UploadOutcome::NetworkError;
  }
  if (!response) return impl_->transport_upload_error(response);

  impl_->note_date(*response);
  const UploadOutcome outcome = classify_upload_status(response->status);
  if (outcome != UploadOutcome::Ok && outcome != UploadOutcome::Duplicate) {
    impl_->last_error = "hub upload_segment returned " +
                        std::to_string(response->status);
  }
  return outcome;
}

Result HubClient::close_event(const std::string& event_id,
                              const EventClosePayload& payload) {
  if (!impl_->ready()) return impl_->unavailable();

  httplib::Result response;
  try {
    response = impl_->client->Post(
        std::string(kApiPrefix) + "/events/" + event_id + "/close",
        event_close_to_json(payload), "application/json");
  } catch (...) {
    return impl_->make_error(Code::Protocol, "hub close_event threw");
  }
  if (!response) return impl_->transport_error(response);

  impl_->note_date(*response);
  if (response->status == 200) return Result::success();
  if (response->status == 404) {
    return impl_->make_error(Code::NotFound, response->body);
  }
  if (response->status == 422) {
    return impl_->make_error(Code::Invalid, response->body);
  }
  return impl_->make_error(
      Code::Protocol,
      "hub close_event returned " + std::to_string(response->status));
}

Result HubClient::cleanup() {
  if (!impl_->ready()) return impl_->unavailable();

  httplib::Result response;
  try {
    response =
        impl_->client->Post(std::string(kApiPrefix) + "/maintenance/cleanup");
  } catch (...) {
    return impl_->make_error(Code::Protocol, "hub cleanup threw");
  }
  if (!response) return impl_->transport_error(response);

  impl_->note_date(*response);
  if (response->status == 200) return Result::success();
  return impl_->make_error(
      Code::Protocol,
      "hub cleanup returned " + std::to_string(response->status));
}

std::optional<int64_t> HubClient::last_server_unix() const {
  return impl_->last_server_unix;
}

const std::string& HubClient::last_error() const { return impl_->last_error; }

std::string HubClient::base_url() const { return impl_->base_url; }

const std::string& HubClient::device_id() const { return impl_->device_id; }

}  // namespace hv
