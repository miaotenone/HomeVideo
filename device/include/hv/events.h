#pragma once

// Event state machine.
//
//   idle --confirm--> Active --silence--> Closing --> Closed
//                     ^ merge window folds adjacent same-type events in
//                     ^ a process that dies mid-event is Interrupted on restart
//
// Design references: §4.3 (person gated by arm mode, debounce, merge), §4.4
// (fire and smoke always on, never motion gated), §5.1 (pre/post roll,
// segmenting), §7.1 (metadata first, video later).
//
// `Closing` is observable only from inside the close handler: it is the state
// between "post-roll expired, final segment flushed" and "handed to the spool".
// The finished record the caller receives is `Closed`.

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "hv/clock.h"
#include "hv/recorder.h"
#include "hv/types.h"

namespace hv {

enum class EventState { Active, Closing, Closed, Interrupted };

const char* to_string(EventState state);

struct EventRecord {
  std::string event_id;
  EventType type = EventType::PersonIntrusion;
  MonotonicMs start_ms = 0;
  std::optional<MonotonicMs> end_ms;
  /** Last detection that belonged to this event; drives the merge window. */
  MonotonicMs last_activity_ms = 0;
  /** Only set when the clock was trusted when the event opened (design §7.2). */
  std::optional<double> started_at_unix;
  bool time_trusted = false;
  std::vector<Detection> detections;
  std::string model_version;
  int config_version = 0;
  ArmMode arm_mode = ArmMode::Unknown;
  EventState state = EventState::Active;

  int64_t duration_ms() const;
  /** Wire spelling of the event type, for the hub and for manifests. */
  const char* type_wire() const { return to_wire(type); }
};

struct EventPolicy {
  int64_t pre_roll_ms = 10000;
  /** How much video is appended after the last detection of an event. */
  int64_t post_roll_ms = 20000;
  /**
   * How long an event stays open after its last detection, so an intermittent
   * burst folds into one event instead of two. Must be >= post_roll_ms (checked
   * by config validation): an event may not close before its own tail is
   * recorded, and a detection arriving before the merge window lapses extends
   * the event rather than opening a second one.
   */
  int64_t merge_window_ms = 30000;
  int64_t segment_ms = 60000;
  size_t segment_bytes = 4ull * 1024 * 1024;
  /** Consecutive qualifying detections needed before an event opens. */
  uint32_t confirm_frames = 3;
  /** ...and they must span at least this long, so one lucky frame never fires. */
  int64_t confirm_hold_ms = 1000;
};

/**
 * Owns the pre-event ring buffer and turns detections into events plus
 * keyframe-aligned segments.
 *
 * Threading: the pipeline calls on_detection from its detection thread and
 * on_chunk from its record thread. The caller serialises the two (the App uses
 * one mutex around both), so this class itself holds no locks.
 *
 * One segmenter per label: at most one event per label is live at a time (the
 * merge window folds repeats), and two labels overlapping writes the shared
 * chunk into both events — each event stays independently decodable.
 */
class EventManager {
 public:
  /** Data is valid only for the duration of the call; move what you keep. */
  using OpenHandler = std::function<void(EventRecord&)>;
  using SegmentHandler = std::function<void(EventRecord&, FinishedSegment&)>;
  using CloseHandler = std::function<void(EventRecord&)>;

  EventManager(EventPolicy policy, std::string device_id, std::string boot_id,
               TimeSource& time, size_t ring_buffer_bytes);

  /**
   * Enables segmenting. Until this is called, events still open and the state
   * machine still runs, but no segments are emitted (the encoder's SPS/PPS are
   * not known yet). The App calls it right after the encoder opens.
   */
  void set_codec_config(std::vector<uint8_t> codec_config);
  bool segmenting_ready() const;

  void set_arm_mode(ArmMode mode, int config_version);
  ArmMode arm_mode() const { return arm_mode_; }
  int config_version() const { return config_version_; }

  void set_model_version(EventType label, std::string version);

  void set_handlers(OpenHandler on_open, SegmentHandler on_segment,
                    CloseHandler on_close,
                    std::function<void()> request_keyframe = {});

  /** Feeds the pre-roll buffer, and appends to any active event's segment. */
  void on_chunk(const EncodedChunk& chunk, MonotonicMs now);

  /** Runs the confirm logic and opens or extends events. */
  void on_detection(const Detection& detection, MonotonicMs now);

  /** Closes events whose post-roll window has expired. */
  void tick(MonotonicMs now);

  const std::vector<EventRecord>& active_events() const { return active_; }

  /** Events that reached Closed since the last call, for upload scheduling. */
  std::vector<EventRecord> take_finished();

  RingBuffer& ring_buffer() { return ring_; }
  uint64_t events_started() const { return events_started_; }
  uint64_t events_closed() const { return events_closed_; }

 private:
  struct Candidate {
    uint32_t frames = 0;
    MonotonicMs first_ms = 0;
    MonotonicMs last_ms = 0;
    /** Detections held while confirming, so the event keeps its evidence. */
    std::vector<Detection> buffered;
  };

  bool qualifies(EventType type) const;
  void open_event(EventType type, const Detection& trigger, MonotonicMs now);
  /** Feeds one chunk into an event's segmenter, emitting finished segments. */
  void feed_segmenter(EventRecord& record, const EncodedChunk& chunk);
  std::optional<Segmenter>& segmenter_for(EventType type);
  void close_event(EventRecord& record, MonotonicMs now, EventState terminal);

  EventPolicy policy_;
  std::string device_id_;
  std::string boot_id_;
  TimeSource& time_;
  RingBuffer ring_;

  std::vector<EventRecord> active_;
  std::vector<EventRecord> finished_;

  std::array<Candidate, kEventTypeCount> candidates_{};
  std::array<std::string, kEventTypeCount> model_versions_{};
  std::array<std::optional<Segmenter>, kEventTypeCount> segmenters_{};
  std::optional<std::vector<uint8_t>> codec_config_;

  ArmMode arm_mode_ = ArmMode::Unknown;
  int config_version_ = 0;
  uint64_t events_started_ = 0;
  uint64_t events_closed_ = 0;
  uint32_t event_seq_ = 0;

  OpenHandler on_open_;
  SegmentHandler on_segment_;
  CloseHandler on_close_;
  std::function<void()> request_keyframe_;
};

}  // namespace hv
