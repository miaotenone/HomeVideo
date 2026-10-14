#include "hv/events.h"

#include <utility>

#include "hv/log.h"

namespace hv {

namespace {

// A detector that keeps firing must not grow an event record without bound: the
// manifest carries the evidence for the hub, and unbounded growth is the one
// way a stuck detector could exhaust the device. Extra detections past the cap
// still extend the merge window; they are just not retained.
constexpr size_t kMaxDetections = 64;

}  // namespace

const char* to_string(EventState state) {
  switch (state) {
    case EventState::Active:
      return "active";
    case EventState::Closing:
      return "closing";
    case EventState::Closed:
      return "closed";
    case EventState::Interrupted:
      return "interrupted";
  }
  return "unknown";
}

int64_t EventRecord::duration_ms() const {
  // end_ms is only set once the event closes; an open (or interrupted) event has
  // no measured duration yet.
  return end_ms ? (*end_ms - start_ms) : 0;
}

EventManager::EventManager(EventPolicy policy, std::string device_id,
                           std::string boot_id, TimeSource& time,
                           size_t ring_buffer_bytes)
    : policy_(policy),
      device_id_(std::move(device_id)),
      boot_id_(std::move(boot_id)),
      time_(time),
      ring_(ring_buffer_bytes) {}

void EventManager::set_codec_config(std::vector<uint8_t> codec_config) {
  codec_config_ = std::move(codec_config);
}

bool EventManager::segmenting_ready() const { return codec_config_.has_value(); }

void EventManager::set_arm_mode(ArmMode mode, int config_version) {
  // Deliberately does not touch events already in progress: the phone can flip
  // home/away mid-burst, and the event that was legitimately confirmed under the
  // old mode must run out its own merge window rather than be closed early
  // (design §4.3).
  arm_mode_ = mode;
  config_version_ = config_version;
}

void EventManager::set_model_version(EventType label, std::string version) {
  model_versions_[index_of(label)] = std::move(version);
}

void EventManager::set_handlers(OpenHandler on_open, SegmentHandler on_segment,
                                CloseHandler on_close,
                                std::function<void()> request_keyframe) {
  on_open_ = std::move(on_open);
  on_segment_ = std::move(on_segment);
  on_close_ = std::move(on_close);
  request_keyframe_ = std::move(request_keyframe);
}

std::optional<Segmenter>& EventManager::segmenter_for(EventType type) {
  auto& slot = segmenters_[index_of(type)];
  // Built lazily because the encoder's SPS/PPS are not known until the encoder
  // opens; until then events still run but emit no video (design §5.1).
  if (!slot && codec_config_) {
    slot.emplace(policy_.segment_ms, policy_.segment_bytes, *codec_config_);
  }
  return slot;
}

bool EventManager::qualifies(EventType type) const {
  const Candidate& candidate = candidates_[index_of(type)];
  // Both conditions are required: N frames alone would let a single burst on one
  // timestamp fire, and the hold alone would let one lucky frame fire.
  return candidate.frames >= policy_.confirm_frames &&
         candidate.last_ms - candidate.first_ms >= policy_.confirm_hold_ms;
}

void EventManager::on_detection(const Detection& detection, MonotonicMs now) {
  const EventType type = detection.label;
  const size_t idx = index_of(type);

  // Person intrusion is only actionable while armed away. Return before touching
  // the candidate so a home/unknown-mode person detection leaves no residue that
  // a later mode flip could mistake for an in-progress run (design §4.3).
  //
  // Fire and smoke are never gated: they must fire in every mode, including
  // Unknown, because a fire is a fire regardless of what the phone reported
  // (design §4.4).
  if (type == EventType::PersonIntrusion && arm_mode_ != ArmMode::Away) {
    return;
  }

  // Merge path: an event for this label is already open, so this detection
  // extends it and must not produce a second on_open (design §4.3).
  for (EventRecord& record : active_) {
    if (record.type != type) continue;
    if (record.detections.size() < kMaxDetections) {
      record.detections.push_back(detection);
    }
    record.last_activity_ms = now;
    return;
  }

  Candidate& candidate = candidates_[idx];
  if (candidate.frames > 0 && now - candidate.last_ms > policy_.confirm_hold_ms) {
    // A gap longer than the required hold means a new run started; frames from
    // before the gap must not help confirm it (design §4.3).
    candidate = Candidate{};
  }
  if (candidate.frames == 0) candidate.first_ms = now;
  candidate.frames++;
  candidate.last_ms = now;
  candidate.buffered.push_back(detection);

  if (qualifies(type)) {
    open_event(type, detection, now);
    candidate = Candidate{};
  }
}

void EventManager::open_event(EventType type, const Detection& trigger,
                              MonotonicMs now) {
  const size_t idx = index_of(type);
  // Read the candidate before the caller resets it: start_ms is the first
  // detection of the confirming run, not the moment confirmation completed, and
  // the buffered run is the event's evidence (design §4.3).
  const Candidate& candidate = candidates_[idx];
  (void)trigger;  // already the last entry of candidate.buffered

  EventRecord record;
  record.event_id = make_event_id(device_id_, boot_id_, now, event_seq_++);
  record.type = type;
  record.start_ms = candidate.first_ms;
  record.last_activity_ms = now;
  record.detections = candidate.buffered;
  // started_at_unix stays empty while the wall clock is untrusted; duration_ms
  // never depends on it (design §7.2).
  record.started_at_unix = time_.now_unix();
  record.time_trusted = time_.trusted();
  record.model_version = model_versions_[idx];
  record.config_version = config_version_;
  record.arm_mode = arm_mode_;
  record.state = EventState::Active;

  active_.push_back(std::move(record));
  events_started_++;
  EventRecord& stored = active_.back();

  // Metadata before video: the handler creates the event directory and manifest
  // first, so a segment can never outrun the event it belongs to (design §7.1).
  if (on_open_) on_open_(stored);

  // Ask for a fresh keyframe now: the encoder's scheduled one may be a full GOP
  // away, and post-event footage must stay decodable (design §5.1).
  if (request_keyframe_) request_keyframe_();

  auto& segmenter = segmenter_for(type);
  if (segmenter) {
    segmenter->reset();
    // Replay the pre-roll one chunk at a time. RingBuffer::since already trims
    // the window to a keyframe, so every segment built from it starts decodable
    // (design §5.1).
    const std::vector<EncodedChunk> pre_roll =
        ring_.since(stored.start_ms - policy_.pre_roll_ms);
    for (const EncodedChunk& chunk : pre_roll) {
      feed_segmenter(stored, chunk);
    }
  }

  HV_DEBUG("event " << stored.event_id << " open type=" << to_wire(type)
                    << " start_ms=" << stored.start_ms);
}

void EventManager::feed_segmenter(EventRecord& record,
                                  const EncodedChunk& chunk) {
  auto& segmenter = segmenter_for(record.type);
  if (!segmenter) return;  // segmenting not ready yet (design §5.1)
  auto finished = segmenter->add(chunk);
  if (finished && on_segment_) on_segment_(record, *finished);
}

void EventManager::on_chunk(const EncodedChunk& chunk, MonotonicMs /*now*/) {
  // now is ignored on purpose: a chunk's own timestamp is what the ring and the
  // segmenter order by, and it survives any scheduling jitter in this call.
  ring_.push(chunk);

  // Two labels overlapping legitimately writes the same chunk into two events;
  // each stays independently decodable and may be closed at a different time.
  for (EventRecord& record : active_) {
    feed_segmenter(record, chunk);
  }
}

void EventManager::tick(MonotonicMs now) {
  // close_event removes the record from active_, so re-scan after each close
  // rather than iterating a vector that is being mutated underneath us.
  for (;;) {
    auto victim = active_.end();
    for (auto it = active_.begin(); it != active_.end(); ++it) {
      if (now - it->last_activity_ms > policy_.merge_window_ms) {
        victim = it;
        break;
      }
    }
    if (victim == active_.end()) break;
    close_event(*victim, now, EventState::Closed);
  }
}

void EventManager::close_event(EventRecord& record, MonotonicMs now,
                               EventState terminal) {
  // The tail runs to post_roll after the last detection. merge_window_ms >=
  // post_roll_ms is guaranteed by config validation, so end_ms is <= now and the
  // tail is fully recorded before the event is closed (design §4.3).
  (void)now;
  record.end_ms = record.last_activity_ms + policy_.post_roll_ms;
  record.state = EventState::Closing;

  // Flush the in-flight segment before announcing the close: an event tail may
  // legitimately not start at a keyframe, and SegmentInfo::keyframe records the
  // truth for the hub (design §5.1).
  auto& segmenter = segmenter_for(record.type);
  if (segmenter) {
    auto tail = segmenter->flush();
    if (tail && on_segment_) on_segment_(record, *tail);
  }

  // The handler observes Closing: the record has not yet been handed to the
  // spool (design §6 lifecycle).
  if (on_close_) on_close_(record);

  record.state = terminal;
  events_closed_++;

  // Leaving active_ before moving into finished_ so no reallocation during the
  // move can leave a dangling slot.
  size_t slot = active_.size();
  for (size_t i = 0; i < active_.size(); ++i) {
    if (&active_[i] == &record) {
      slot = i;
      break;
    }
  }
  HV_DEBUG("event " << record.event_id << " closed state=" << to_string(terminal)
                    << " duration_ms=" << record.duration_ms());
  finished_.push_back(std::move(record));
  if (slot < active_.size()) {
    active_.erase(active_.begin() + static_cast<ptrdiff_t>(slot));
  }
}

std::vector<EventRecord> EventManager::take_finished() {
  // Handed over exactly once: the upload scheduler owns the record after this.
  std::vector<EventRecord> out = std::move(finished_);
  finished_.clear();
  return out;
}

}  // namespace hv
