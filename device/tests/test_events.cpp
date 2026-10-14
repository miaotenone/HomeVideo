#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "harness.h"
#include "hv/clock.h"
#include "hv/events.h"
#include "hv/recorder.h"
#include "hv/types.h"

using namespace hv;

namespace {

// A recognisable Annex-B SPS-ish prefix: any emitted keyframe-led segment must
// start with exactly these bytes (design §5.2 self-describing segments).
const std::vector<uint8_t> kCodecConfig = {0x00, 0x00, 0x00, 0x01,
                                           0x67, 0x42, 0x00, 0x1e};

// 15 fps: frame `index` lands at this offset, keyframe every 30 frames.
MonotonicMs frame_ts(int index) {
  return static_cast<MonotonicMs>(index) * 1000 / 15;
}

EncodedChunk make_chunk(MonotonicMs ts_ms, bool keyframe, size_t size = 64) {
  EncodedChunk chunk;
  chunk.ts_ms = ts_ms;
  chunk.keyframe = keyframe;
  // Fill differs per timestamp so a mis-ordered stream is visible in failures.
  chunk.data.assign(size, static_cast<uint8_t>(0x10 + ((ts_ms / 100) % 7)));
  return chunk;
}

Detection make_detection(EventType label, MonotonicMs ts_ms, float conf = 0.9f) {
  Detection detection;
  detection.label = label;
  detection.confidence = conf;
  detection.ts_ms = ts_ms;
  return detection;
}

bool begins_with(const std::vector<uint8_t>& data,
                 const std::vector<uint8_t>& prefix) {
  return data.size() >= prefix.size() &&
         std::equal(prefix.begin(), prefix.end(), data.begin());
}

size_t first_index(const std::vector<std::string>& order, const char* what) {
  for (size_t i = 0; i < order.size(); ++i) {
    if (order[i] == what) return i;
  }
  return order.size();
}

// Records every handler invocation so tests can assert on call order and
// payloads rather than poking at private state.
struct Fixture {
  FakeClock clock;
  TimeSource time{clock};
  EventPolicy policy;

  std::vector<std::string> open_ids;
  std::vector<EventRecord> open_records;
  std::vector<EventState> close_states;
  std::vector<EventRecord> closed_records;
  std::vector<std::string> segment_event_ids;
  std::vector<FinishedSegment> segments;
  std::vector<std::string> order;
  int keyframe_requests = 0;

  EventManager manager;

  explicit Fixture(EventPolicy configured = EventPolicy{},
                   size_t ring_bytes = 8u << 20)
      : policy(configured),
        manager(policy, "v831a", "boot01", time, ring_bytes) {}

  void install() {
    manager.set_handlers(
        [this](EventRecord& record) {
          open_ids.push_back(record.event_id);
          open_records.push_back(record);
          order.push_back("open");
        },
        [this](EventRecord& record, FinishedSegment& segment) {
          segment_event_ids.push_back(record.event_id);
          segments.push_back(segment);
          order.push_back("segment");
        },
        [this](EventRecord& record) {
          close_states.push_back(record.state);
          closed_records.push_back(record);
          order.push_back("close");
        },
        [this]() { ++keyframe_requests; });
  }
};

void push_frame(Fixture& fx, int index, size_t size = 64) {
  fx.manager.on_chunk(make_chunk(frame_ts(index), index % 30 == 0, size),
                      frame_ts(index));
}

// Three detections 500 ms apart span exactly `confirm_hold_ms` (1000 ms), the
// minimum a run must cover, so this opens one event for `label`.
void confirm_run(Fixture& fx, EventType label, MonotonicMs start_ms) {
  fx.manager.on_detection(make_detection(label, start_ms), start_ms);
  fx.manager.on_detection(make_detection(label, start_ms + 500), start_ms + 500);
  fx.manager.on_detection(make_detection(label, start_ms + 1000),
                          start_ms + 1000);
}

}  // namespace

HV_TEST(events_confirm_needs_frames_and_hold) {
  Fixture fx;
  fx.install();
  const EventType label = EventType::FireSuspected;

  // One frame on its own never fires.
  fx.manager.on_detection(make_detection(label, 0), 0);
  HV_CHECK_EQ(fx.manager.events_started(), uint64_t{0});
  HV_CHECK(fx.manager.active_events().empty());

  // Two of three frames is still not enough.
  fx.manager.on_detection(make_detection(label, 500), 500);
  HV_CHECK_EQ(fx.manager.events_started(), uint64_t{0});

  // The third frame completes both the count and the hold: exactly one event.
  fx.manager.on_detection(make_detection(label, 1000), 1000);
  HV_CHECK_EQ(fx.manager.events_started(), uint64_t{1});
  HV_CHECK_EQ(fx.manager.active_events().size(), size_t{1});
  HV_CHECK_EQ(fx.manager.active_events().front().start_ms, MonotonicMs{0});
  HV_CHECK_EQ(fx.open_ids.size(), size_t{1});
}

HV_TEST(events_confirm_requires_the_full_hold) {
  Fixture fx;
  fx.install();
  const EventType label = EventType::FireSuspected;

  // Six consecutive frames, every 100 ms: the count is met but the run spans
  // only 500 ms, so the debounce must hold it back.
  for (int i = 0; i < 6; ++i) {
    const MonotonicMs ts = static_cast<MonotonicMs>(i) * 100;
    fx.manager.on_detection(make_detection(label, ts), ts);
  }
  HV_CHECK_EQ(fx.manager.events_started(), uint64_t{0});
  HV_CHECK(fx.manager.active_events().empty());
}

HV_TEST(events_gap_longer_than_hold_resets_the_run) {
  Fixture fx;
  fx.install();
  const EventType label = EventType::FireSuspected;

  fx.manager.on_detection(make_detection(label, 0), 0);
  fx.manager.on_detection(make_detection(label, 100), 100);

  // 1.9 s of silence exceeds confirm_hold_ms: the two frames above are a run that
  // never confirmed and must be forgotten (design §4.3).
  fx.manager.on_detection(make_detection(label, 2000), 2000);
  fx.manager.on_detection(make_detection(label, 2500), 2500);
  HV_CHECK_EQ(fx.manager.events_started(), uint64_t{0});

  // If the pre-gap frames still counted this would be frame 5 and would open.
  fx.manager.on_detection(make_detection(label, 3000), 3000);
  HV_CHECK_EQ(fx.manager.events_started(), uint64_t{1});
  HV_CHECK_EQ(fx.manager.active_events().front().start_ms, MonotonicMs{2000});
}

HV_TEST(events_person_is_gated_by_arm_mode) {
  Fixture fx;
  fx.install();

  // Home and Unknown must not act on a person detection at all.
  fx.manager.set_arm_mode(ArmMode::Home, 1);
  confirm_run(fx, EventType::PersonIntrusion, 0);
  HV_CHECK_EQ(fx.manager.events_started(), uint64_t{0});

  fx.manager.set_arm_mode(ArmMode::Unknown, 2);
  confirm_run(fx, EventType::PersonIntrusion, 5000);
  HV_CHECK_EQ(fx.manager.events_started(), uint64_t{0});
  HV_CHECK(fx.manager.active_events().empty());

  // Away arms the person path.
  fx.manager.set_arm_mode(ArmMode::Away, 3);
  confirm_run(fx, EventType::PersonIntrusion, 10000);
  HV_CHECK_EQ(fx.manager.events_started(), uint64_t{1});
  HV_CHECK_EQ(fx.manager.arm_mode(), ArmMode::Away);
  HV_CHECK_EQ(fx.manager.config_version(), 3);
  HV_CHECK_EQ(fx.manager.active_events().front().arm_mode, ArmMode::Away);
  HV_CHECK_EQ(fx.manager.active_events().front().config_version, 3);
}

HV_TEST(events_fire_and_smoke_open_in_every_mode) {
  const ArmMode modes[] = {ArmMode::Home, ArmMode::Away, ArmMode::Unknown};
  for (const ArmMode mode : modes) {
    Fixture fire;
    fire.install();
    fire.manager.set_arm_mode(mode, 1);
    confirm_run(fire, EventType::FireSuspected, 0);
    HV_CHECK_EQ(fire.manager.events_started(), uint64_t{1});

    Fixture smoke;
    smoke.install();
    smoke.manager.set_arm_mode(mode, 1);
    confirm_run(smoke, EventType::SmokeSuspected, 0);
    HV_CHECK_EQ(smoke.manager.events_started(), uint64_t{1});
  }
}

HV_TEST(events_open_precedes_segments) {
  EventPolicy policy;
  // Roll at every keyframe so the ordering shows up immediately, not only at
  // close.
  policy.segment_ms = 0;
  Fixture fx(policy);
  fx.install();
  fx.manager.set_codec_config(kCodecConfig);

  confirm_run(fx, EventType::FireSuspected, 0);
  for (int i = 0; i < 3; ++i) push_frame(fx, i * 30);

  HV_CHECK(!fx.segments.empty());
  const size_t open_at = first_index(fx.order, "open");
  const size_t segment_at = first_index(fx.order, "segment");
  HV_CHECK(segment_at < fx.order.size());
  HV_CHECK(open_at < segment_at);
}

HV_TEST(events_replays_pre_roll_from_a_keyframe) {
  EventPolicy policy;
  policy.pre_roll_ms = 10000;
  Fixture fx(policy);
  fx.install();
  fx.manager.set_codec_config(kCodecConfig);

  // 10 s of encoded video buffered before the event: 151 frames at 15 fps with a
  // keyframe every 30 frames.
  for (int i = 0; i <= 150; ++i) push_frame(fx, i);

  confirm_run(fx, EventType::FireSuspected, 10000);  // start_ms == 10000
  fx.manager.tick(11000 + policy.merge_window_ms + 1);

  HV_CHECK_EQ(fx.closed_records.size(), size_t{1});
  HV_CHECK(!fx.segments.empty());
  const EventRecord& closed = fx.closed_records.front();
  const FinishedSegment& first = fx.segments.front();

  // The window was trimmed forward to a keyframe, so the segment starts no later
  // than the event and is independently decodable (design §5.1).
  HV_CHECK(first.info.start_ms <= closed.start_ms);
  HV_CHECK(first.info.keyframe);
  HV_CHECK(begins_with(first.data, kCodecConfig));
}

HV_TEST(events_no_segments_until_codec_config_arrives) {
  Fixture fx;
  fx.install();

  // Before the encoder publishes SPS/PPS the event still runs end to end, it
  // just produces no video (design §5.1).
  confirm_run(fx, EventType::FireSuspected, 0);
  fx.manager.on_chunk(make_chunk(0, true), 0);
  fx.manager.on_chunk(make_chunk(2000, true), 2000);
  HV_CHECK_FALSE(fx.manager.segmenting_ready());
  HV_CHECK(fx.segments.empty());

  fx.manager.tick(1000 + fx.policy.merge_window_ms + 1);
  HV_CHECK_EQ(fx.open_ids.size(), size_t{1});
  HV_CHECK_EQ(fx.closed_records.size(), size_t{1});
  HV_CHECK(fx.segments.empty());

  // A later event, opened after set_codec_config, does segment.
  fx.manager.set_codec_config(kCodecConfig);
  HV_CHECK_TRUE(fx.manager.segmenting_ready());
  confirm_run(fx, EventType::FireSuspected, 100000);
  fx.manager.on_chunk(make_chunk(100000, true), 100000);
  fx.manager.on_chunk(make_chunk(102000, true), 102000);
  fx.manager.tick(101000 + fx.policy.merge_window_ms + 1);
  HV_CHECK(!fx.segments.empty());
}

HV_TEST(events_tick_closes_after_the_merge_window) {
  EventPolicy policy;
  // merge == post so the close boundary is unambiguous; config guarantees
  // merge_window_ms >= post_roll_ms.
  policy.merge_window_ms = 5000;
  policy.post_roll_ms = 5000;
  Fixture fx(policy);
  fx.install();

  confirm_run(fx, EventType::FireSuspected, 0);  // start 0, last activity 1000

  // Exactly at the window the event is still open (the comparison is strict).
  fx.manager.tick(1000 + policy.merge_window_ms);
  HV_CHECK_EQ(fx.manager.active_events().size(), size_t{1});
  HV_CHECK_EQ(fx.closed_records.size(), size_t{0});

  fx.manager.tick(1000 + policy.merge_window_ms + 1);
  HV_CHECK_EQ(fx.closed_records.size(), size_t{1});
  HV_CHECK_EQ(fx.close_states.front(), EventState::Closing);

  // The record the close handler saw was Closing; the record handed over after
  // the transition is Closed.
  std::vector<EventRecord> finished = fx.manager.take_finished();
  HV_CHECK_EQ(finished.size(), size_t{1});
  const EventRecord& closed = finished.front();
  HV_CHECK_EQ(closed.state, EventState::Closed);
  HV_CHECK_EQ(closed.last_activity_ms, MonotonicMs{1000});
  HV_CHECK_EQ(closed.end_ms.value_or(-1), MonotonicMs{6000});
  HV_CHECK_EQ(closed.end_ms.value_or(-1),
              closed.last_activity_ms + policy.post_roll_ms);
  HV_CHECK_EQ(closed.duration_ms(), closed.end_ms.value_or(-1) - closed.start_ms);
  HV_CHECK_EQ(closed.duration_ms(), MonotonicMs{6000});

  // Nothing is left to close on a second tick.
  fx.manager.tick(1000 + 2 * policy.merge_window_ms);
  HV_CHECK_EQ(fx.closed_records.size(), size_t{1});
  HV_CHECK(fx.manager.active_events().empty());
}

HV_TEST(events_detection_before_merge_window_extends) {
  Fixture fx;
  fx.install();
  confirm_run(fx, EventType::FireSuspected, 0);
  HV_CHECK_EQ(fx.open_ids.size(), size_t{1});

  const MonotonicMs within = 1000 + fx.policy.merge_window_ms;
  fx.manager.tick(within);  // not yet past: the event stays open
  HV_CHECK_EQ(fx.manager.active_events().size(), size_t{1});

  fx.manager.on_detection(make_detection(EventType::FireSuspected, within),
                          within);
  HV_CHECK_EQ(fx.manager.events_started(), uint64_t{1});
  HV_CHECK_EQ(fx.open_ids.size(), size_t{1});

  const EventRecord& record = fx.manager.active_events().front();
  HV_CHECK_EQ(record.last_activity_ms, within);
  HV_CHECK_EQ(record.detections.size(), size_t{4});
}

HV_TEST(events_after_close_a_new_detection_opens_a_second) {
  EventPolicy policy;
  policy.merge_window_ms = 5000;
  policy.post_roll_ms = 5000;
  Fixture fx(policy);
  fx.install();

  confirm_run(fx, EventType::FireSuspected, 0);
  fx.manager.tick(6001);
  HV_CHECK_EQ(fx.closed_records.size(), size_t{1});

  confirm_run(fx, EventType::FireSuspected, 20000);
  HV_CHECK_EQ(fx.manager.events_started(), uint64_t{2});
  HV_CHECK_EQ(fx.open_ids.size(), size_t{2});
  HV_CHECK_NE(fx.open_ids[0], fx.open_ids[1]);
}

HV_TEST(events_two_labels_segment_from_the_same_stream) {
  EventPolicy policy;
  policy.segment_ms = 0;
  Fixture fx(policy);
  fx.install();
  fx.manager.set_codec_config(kCodecConfig);
  fx.manager.set_arm_mode(ArmMode::Away, 1);

  confirm_run(fx, EventType::FireSuspected, 0);
  confirm_run(fx, EventType::PersonIntrusion, 2000);
  HV_CHECK_EQ(fx.open_ids.size(), size_t{2});
  HV_CHECK_NE(fx.open_ids[0], fx.open_ids[1]);

  // One shared stream, both events active: each gets its own decodable segment.
  fx.manager.on_chunk(make_chunk(0, true), 0);
  fx.manager.on_chunk(make_chunk(2000, true), 2000);

  HV_CHECK_EQ(fx.segment_event_ids.size(), size_t{2});
  HV_CHECK_STR_EQ(fx.segment_event_ids[0], fx.open_ids[0]);
  HV_CHECK_STR_EQ(fx.segment_event_ids[1], fx.open_ids[1]);
}

HV_TEST(events_time_trust_controls_started_at) {
  EventPolicy policy;
  policy.merge_window_ms = 5000;
  policy.post_roll_ms = 5000;
  Fixture fx(policy);
  fx.install();

  HV_CHECK_FALSE(fx.time.trusted());
  confirm_run(fx, EventType::FireSuspected, 0);
  const EventRecord& open = fx.manager.active_events().front();
  HV_CHECK_FALSE(open.started_at_unix.has_value());
  HV_CHECK_FALSE(open.time_trusted);

  // Duration is monotonic and survives an untrusted wall clock (design §7.2).
  fx.manager.tick(6001);
  const EventRecord& closed = fx.closed_records.front();
  HV_CHECK_FALSE(closed.started_at_unix.has_value());
  HV_CHECK_FALSE(closed.time_trusted);
  HV_CHECK_EQ(closed.duration_ms(), MonotonicMs{6000});

  // The hub's Date header is seen: later events carry a wall-clock start.
  fx.time.observe_server_unix(fx.clock.unix_seconds());
  HV_CHECK_TRUE(fx.time.trusted());

  confirm_run(fx, EventType::FireSuspected, 20000);
  const EventRecord& later = fx.manager.active_events().front();
  HV_CHECK_TRUE(later.time_trusted);
  HV_CHECK(later.started_at_unix.has_value());
}

HV_TEST(events_take_finished_hands_over_once) {
  EventPolicy policy;
  policy.merge_window_ms = 5000;
  policy.post_roll_ms = 5000;
  Fixture fx(policy);
  fx.install();

  confirm_run(fx, EventType::FireSuspected, 0);
  fx.manager.tick(6001);

  std::vector<EventRecord> finished = fx.manager.take_finished();
  HV_CHECK_EQ(finished.size(), size_t{1});
  HV_CHECK_EQ(finished.front().state, EventState::Closed);
  HV_CHECK(fx.manager.take_finished().empty());
}

HV_TEST(events_active_shrinks_after_all_close) {
  EventPolicy policy;
  policy.merge_window_ms = 5000;
  policy.post_roll_ms = 5000;
  Fixture fx(policy);
  fx.install();
  fx.manager.set_arm_mode(ArmMode::Away, 1);

  confirm_run(fx, EventType::FireSuspected, 0);
  confirm_run(fx, EventType::PersonIntrusion, 2000);
  HV_CHECK_EQ(fx.manager.active_events().size(), size_t{2});

  fx.manager.tick(10000);
  HV_CHECK(fx.manager.active_events().empty());
  HV_CHECK_EQ(fx.manager.events_closed(), uint64_t{2});
}
