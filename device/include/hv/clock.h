#pragma once

// Clocks and time-trust.
//
// Design §7.2 / §9: durations and every `*_ms` field use the monotonic clock, so
// a system-time correction can never change a measured duration. The wall clock
// (and therefore `started_at` / `ended_at`) is only used when it is trusted.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

#include "hv/types.h"

namespace hv {

class IClock {
 public:
  virtual ~IClock() = default;
  virtual MonotonicMs monotonic_ms() const = 0;
  virtual int64_t unix_seconds() const = 0;
};

/** Production clock: steady_clock for monotonic, system_clock for wall time. */
class SystemClock : public IClock {
 public:
  SystemClock();
  MonotonicMs monotonic_ms() const override;
  int64_t unix_seconds() const override;

 private:
  std::chrono::steady_clock::time_point anchor_;
};

/** Test clock: manually advanced, can also telegraph a wall-clock jump. */
class FakeClock : public IClock {
 public:
  explicit FakeClock(int64_t unix_seconds = 1760000000) : unix_(unix_seconds) {}
  MonotonicMs monotonic_ms() const override { return monotonic_ms_; }
  int64_t unix_seconds() const override { return unix_; }

  /** Advances both clocks together, as real time does. */
  void advance_ms(MonotonicMs delta_ms);
  /** Moves the wall clock only — an NTP-style step. */
  void jump_wall_seconds(int64_t delta_seconds) { unix_ += delta_seconds; }
  void set_unix_seconds(int64_t value) { unix_ = value; }

 private:
  MonotonicMs monotonic_ms_ = 0;
  int64_t unix_ = 0;
  int64_t wall_ms_ = 0;
};

/**
 * Tracks whether the wall clock can be trusted.
 *
 * The hub returns no explicit time field, so the device reads the HTTP `Date`
 * header of any hub response (heartbeat is the usual one). A skew larger than
 * `max_skew_seconds` leaves the clock untrusted rather than silently stepping it.
 */
class TimeSource {
 public:
  explicit TimeSource(IClock& clock) : clock_(clock) {}

  void observe_server_unix(int64_t server_unix, int64_t max_skew_seconds = 300);
  bool trusted() const { return trusted_; }

  /** Wall clock "now", extrapolated from the last observation; empty if untrusted. */
  std::optional<double> now_unix() const;

  MonotonicMs uptime_ms() const { return clock_.monotonic_ms(); }

 private:
  IClock& clock_;
  bool trusted_ = false;
  int64_t base_server_unix_ = 0;
  MonotonicMs base_monotonic_ms_ = 0;
};

/** 8 hex chars identifying this process run. */
std::string new_boot_id();

/**
 * Builds an event id that satisfies the hub's `^[A-Za-z0-9._-]{1,128}$` rule:
 * "<device>-<boot>-<uptime_ms>-<seq>". Disallowed characters become '_'.
 */
std::string make_event_id(const std::string& device_id, const std::string& boot_id,
                          MonotonicMs uptime_ms, uint32_t seq);

}  // namespace hv
