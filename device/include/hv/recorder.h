#pragma once

// Recording primitives: pre-event ring buffer and keyframe-aligned segmenting.
//
// Memory contract (design §5.2): the ring buffer is the only always-on video
// buffer, and the segmenter holds at most one in-flight segment. Segmenting
// rolls at a keyframe once EITHER limit is reached, so the byte cap is honoured
// to within one GOP instead of one whole segment period.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "hv/status.h"
#include "hv/types.h"

namespace hv {

/** Lowercase hex sha256. Self-contained: no OpenSSL on the musl target. */
std::string sha256_hex(const uint8_t* data, size_t size);
std::string sha256_hex(const std::vector<uint8_t>& data);

/** Bounded, byte-capped window of recently encoded chunks. */
class RingBuffer {
 public:
  explicit RingBuffer(size_t max_bytes);

  void push(EncodedChunk chunk);

  /**
   * Chunks with `ts_ms >= since_ms`, oldest first. Leading non-keyframes are
   * dropped so a recording can always start at a decodable keyframe.
   */
  std::vector<EncodedChunk> since(MonotonicMs since_ms) const;

  size_t bytes() const;
  size_t chunks() const;
  uint64_t evicted_chunks() const;
  uint64_t evicted_bytes() const;
  void clear();

 private:
  size_t max_bytes_;
  size_t bytes_ = 0;
  uint64_t evicted_chunks_ = 0;
  uint64_t evicted_bytes_ = 0;
  std::vector<EncodedChunk> chunks_;
};

struct SegmentInfo {
  int seq = 0;
  std::string file;  // seg_0000.bin
  uint64_t bytes = 0;
  std::string sha256;
  bool keyframe = false;  // starts at a keyframe (see flush() caveat)
  MonotonicMs start_ms = 0;
  MonotonicMs end_ms = 0;
  bool uploaded = false;
};

struct FinishedSegment {
  SegmentInfo info;
  std::vector<uint8_t> data;  // exactly what was hashed and what gets written
};

/**
 * Turns a chunk stream into keyframe-aligned segments.
 *
 * `add` returns a finished segment when the current one rolls; `flush` finishes
 * whatever is buffered (used when an event closes, where the last segment may
 * legitimately not start at a keyframe — `SegmentInfo::keyframe` says so).
 * Codec configuration is prepended to a segment whose first chunk is a keyframe
 * and does not already start with it, so segments are self-describing.
 */
class Segmenter {
 public:
  Segmenter(MonotonicMs segment_ms, size_t max_segment_bytes,
            std::vector<uint8_t> codec_config);

  std::optional<FinishedSegment> add(const EncodedChunk& chunk);
  std::optional<FinishedSegment> flush();

  int next_seq() const { return next_seq_; }
  uint64_t buffered_bytes() const { return buffered_bytes_; }
  bool has_pending() const { return !pending_.empty(); }

  /** Drops buffered chunks without emitting, e.g. when an event is abandoned. */
  void discard();

  /** Discards and restarts sequence numbering at 0, for a fresh event. */
  void reset();

 private:
  std::optional<FinishedSegment> emit(bool starts_at_keyframe);

  MonotonicMs segment_ms_;
  size_t max_segment_bytes_;
  std::vector<uint8_t> codec_config_;
  std::vector<EncodedChunk> pending_;
  uint64_t buffered_bytes_ = 0;
  int next_seq_ = 0;
};

}  // namespace hv
