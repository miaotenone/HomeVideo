#include "hv/clock.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>

namespace hv {

SystemClock::SystemClock() : anchor_(std::chrono::steady_clock::now()) {
  // Anchor the monotonic origin at construction so monotonic_ms() reads as uptime.
}

MonotonicMs SystemClock::monotonic_ms() const {
  const auto delta = std::chrono::steady_clock::now() - anchor_;
  return std::chrono::duration_cast<std::chrono::milliseconds>(delta).count();
}

int64_t SystemClock::unix_seconds() const {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

void FakeClock::advance_ms(MonotonicMs delta_ms) {
  monotonic_ms_ += delta_ms;
  wall_ms_ += delta_ms;
  unix_ += wall_ms_ / 1000;
  wall_ms_ %= 1000;
}

void TimeSource::observe_server_unix(int64_t server_unix, int64_t max_skew_seconds) {
  const int64_t local = clock_.unix_seconds();
  const int64_t skew = server_unix - local;
  const int64_t magnitude = skew < 0 ? -skew : skew;

  if (magnitude > max_skew_seconds) {
    trusted_ = false;
    return;
  }

  trusted_ = true;
  base_server_unix_ = server_unix;
  base_monotonic_ms_ = clock_.monotonic_ms();
}

std::optional<double> TimeSource::now_unix() const {
  if (!trusted_) return std::nullopt;
  const MonotonicMs elapsed = clock_.monotonic_ms() - base_monotonic_ms_;
  return static_cast<double>(base_server_unix_) +
         static_cast<double>(elapsed) / 1000.0;
}

std::string new_boot_id() {
  std::random_device device;
  std::uniform_int_distribution<uint32_t> dist;
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%08x", dist(device));
  return std::string(buffer);
}

namespace {

std::string sanitize_component(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (const char c : value) {
    const bool allowed = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                         (c >= 'A' && c <= 'Z') || c == '.' || c == '_' ||
                         c == '-';
    out.push_back(allowed ? c : '_');
  }
  return out;
}

}  // namespace

std::string make_event_id(const std::string& device_id, const std::string& boot_id,
                          MonotonicMs uptime_ms, uint32_t seq) {
  std::string device = sanitize_component(device_id);
  if (device.empty()) device = "dev";
  std::string boot = sanitize_component(boot_id);

  char tail[64];
  std::snprintf(tail, sizeof(tail), "-%lld-%04u",
                static_cast<long long>(uptime_ms), seq % 10000);
  const size_t tail_len = std::strlen(tail);

  // Reserve characters for "<boot><tail>" plus the two separators.
  const size_t reserved = boot.size() + tail_len + 2;
  constexpr size_t kMaxLength = 128;
  if (reserved >= kMaxLength) {
    // Pathological inputs: keep the tail, which is what makes ids unique.
    std::string out = "d-";
    out += boot.substr(0, kMaxLength - 2 - tail_len);
    out += tail;
    return out;
  }
  if (device.size() + reserved > kMaxLength) {
    device.resize(kMaxLength - reserved);
  }
  if (device.empty()) device = "d";

  return device + "-" + boot + tail;
}

}  // namespace hv
