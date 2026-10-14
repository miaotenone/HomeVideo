#include <regex>
#include <string>

#include "harness.h"
#include "hv/clock.h"

using namespace hv;

namespace {

bool matches_event_id_rule(const std::string& value) {
  static const std::regex rule("^[A-Za-z0-9._-]{1,128}$");
  return std::regex_match(value, rule);
}

}  // namespace

HV_TEST(system_clock_never_goes_backwards) {
  SystemClock clock;
  MonotonicMs previous = clock.monotonic_ms();
  HV_CHECK(previous >= 0);
  for (int i = 0; i < 3; ++i) {
    const MonotonicMs now = clock.monotonic_ms();
    HV_CHECK(now >= previous);
    previous = now;
  }
  HV_CHECK(clock.unix_seconds() > 1600000000);
}

HV_TEST(fake_clock_wall_jump_leaves_monotonic_alone) {
  FakeClock clock;
  clock.advance_ms(5000);
  const MonotonicMs before = clock.monotonic_ms();
  const int64_t wall_before = clock.unix_seconds();

  // An NTP-style step of one hour must not disturb durations.
  clock.jump_wall_seconds(3600);

  HV_CHECK_EQ(clock.monotonic_ms(), before);
  HV_CHECK_EQ(clock.unix_seconds(), wall_before + 3600);
}

HV_TEST(time_source_is_untrusted_until_a_server_time_is_seen) {
  FakeClock clock;
  TimeSource source(clock);
  HV_CHECK_FALSE(source.trusted());
  HV_CHECK_FALSE(source.now_unix().has_value());
}

HV_TEST(time_source_trusts_small_skew_and_extrapolates) {
  FakeClock clock;
  TimeSource source(clock);
  const int64_t server = clock.unix_seconds() + 2;
  source.observe_server_unix(server);
  HV_CHECK_TRUE(source.trusted());
  HV_CHECK(source.now_unix().has_value());
  HV_CHECK_NEAR(source.now_unix().value(), static_cast<double>(server), 0.001);

  clock.advance_ms(1500);
  HV_CHECK_NEAR(source.now_unix().value(), static_cast<double>(server) + 1.5, 0.001);
  HV_CHECK_EQ(source.uptime_ms(), MonotonicMs{1500});
}

HV_TEST(time_source_rejects_large_skew) {
  FakeClock clock;
  TimeSource source(clock);
  source.observe_server_unix(clock.unix_seconds() - 3600);
  HV_CHECK_FALSE(source.trusted());
  HV_CHECK_FALSE(source.now_unix().has_value());

  // Once the clock is back within tolerance it becomes trusted again.
  source.observe_server_unix(clock.unix_seconds() + 1);
  HV_CHECK_TRUE(source.trusted());
}

HV_TEST(boot_id_is_eight_hex_digits) {
  const std::string boot = new_boot_id();
  HV_CHECK_EQ(boot.size(), size_t{8});
  HV_CHECK(std::regex_match(boot, std::regex("^[0-9a-f]{8}$")));
}

HV_TEST(event_ids_match_the_hub_filename_rule) {
  const std::string id = make_event_id("v831a", "3f9c1d2e", 173456, 7);
  HV_CHECK(matches_event_id_rule(id));
  HV_CHECK_STR_EQ(id, "v831a-3f9c1d2e-173456-0007");
}

HV_TEST(event_ids_survive_hostile_device_ids) {
  const std::string hostile(300, 'x');
  const std::string id = make_event_id(hostile + "/../etc", "boot", 1, 1);
  HV_CHECK(matches_event_id_rule(id));
  HV_CHECK(id.size() <= 128);
}

HV_TEST(event_ids_differ_per_sequence) {
  const std::string a = make_event_id("v831a", "deadbeef", 1000, 1);
  const std::string b = make_event_id("v831a", "deadbeef", 1000, 2);
  HV_CHECK_NE(a, b);
}
