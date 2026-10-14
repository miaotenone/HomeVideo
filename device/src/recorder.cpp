#include "hv/recorder.h"

#include <cstdio>
#include <cstring>
#include <utility>

namespace hv {

namespace {

// SHA-256 (FIPS 180-4). Hand-rolled because the musl board target has no
// OpenSSL and the build must not gain a dependency for one digest.

constexpr uint32_t kRoundConstant[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr uint32_t kInitialState[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372,
                                       0xa54ff53a, 0x510e527f, 0x9b05688c,
                                       0x1f83d9ab, 0x5be0cd19};

inline uint32_t rotr(uint32_t value, int bits) {
  return (value >> bits) | (value << (32 - bits));
}

void sha256_compress(uint32_t state[8], const uint8_t block[64]) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    const size_t at = static_cast<size_t>(i) * 4;
    w[i] = (static_cast<uint32_t>(block[at]) << 24) |
           (static_cast<uint32_t>(block[at + 1]) << 16) |
           (static_cast<uint32_t>(block[at + 2]) << 8) |
           static_cast<uint32_t>(block[at + 3]);
  }
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 =
        rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 =
        rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
  uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
  for (int i = 0; i < 64; ++i) {
    const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const uint32_t ch = (e & f) ^ (~e & g);
    const uint32_t temp1 = h + s1 + ch + kRoundConstant[i] + w[i];
    const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t temp2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

bool starts_with(const std::vector<uint8_t>& data,
                 const std::vector<uint8_t>& prefix) {
  if (prefix.size() > data.size()) return false;
  return std::memcmp(data.data(), prefix.data(), prefix.size()) == 0;
}

}  // namespace

std::string sha256_hex(const uint8_t* data, size_t size) {
  uint32_t state[8];
  std::memcpy(state, kInitialState, sizeof(state));

  size_t offset = 0;
  while (size - offset >= 64) {
    sha256_compress(state, data + offset);
    offset += 64;
  }

  // Final one or two blocks: 0x80, zero padding, then the 64-bit bit length.
  uint8_t tail[128];
  std::memset(tail, 0, sizeof(tail));
  const size_t remaining = size - offset;
  if (remaining > 0) {
    std::memcpy(tail, data + offset, remaining);
  }
  tail[remaining] = 0x80;

  const uint64_t bits = static_cast<uint64_t>(size) * 8;
  const size_t tail_len = remaining + 1 + 8 <= 64 ? 64 : 128;
  for (size_t i = 0; i < 8; ++i) {
    tail[tail_len - 1 - i] = static_cast<uint8_t>(bits >> (8 * i));
  }

  sha256_compress(state, tail);
  if (tail_len == 128) sha256_compress(state, tail + 64);

  static const char kHex[] = "0123456789abcdef";
  std::string out(64, '0');
  for (size_t i = 0; i < 8; ++i) {
    for (size_t byte = 0; byte < 4; ++byte) {
      const uint8_t value =
          static_cast<uint8_t>(state[i] >> (24 - static_cast<int>(8 * byte)));
      out[(i * 4 + byte) * 2] = kHex[value >> 4];
      out[(i * 4 + byte) * 2 + 1] = kHex[value & 0x0f];
    }
  }
  return out;
}

std::string sha256_hex(const std::vector<uint8_t>& data) {
  return sha256_hex(data.data(), data.size());
}

RingBuffer::RingBuffer(size_t max_bytes) : max_bytes_(max_bytes) {}

void RingBuffer::push(EncodedChunk chunk) {
  bytes_ += chunk.data.size();
  chunks_.push_back(std::move(chunk));

  while (bytes_ > max_bytes_) {
    // A single chunk over the cap is kept: evicting it would throw away the only
    // video data available during an encoding stall, and one chunk is already
    // the smallest state this buffer can hold.
    if (chunks_.size() <= 1) break;

    const size_t dropped = chunks_.front().data.size();
    bytes_ -= dropped;
    evicted_bytes_ += dropped;
    evicted_chunks_ += 1;
    chunks_.erase(chunks_.begin());
  }
}

std::vector<EncodedChunk> RingBuffer::since(MonotonicMs since_ms) const {
  size_t first = 0;
  while (first < chunks_.size() && chunks_[first].ts_ms < since_ms) ++first;

  // Skip forward to the first keyframe: a clip that starts on a delta frame is
  // not decodable on its own (design §5.1).
  while (first < chunks_.size() && !chunks_[first].keyframe) ++first;
  if (first >= chunks_.size()) return {};

  return std::vector<EncodedChunk>(chunks_.begin() + static_cast<ptrdiff_t>(first),
                                   chunks_.end());
}

size_t RingBuffer::bytes() const { return bytes_; }
size_t RingBuffer::chunks() const { return chunks_.size(); }
uint64_t RingBuffer::evicted_chunks() const { return evicted_chunks_; }
uint64_t RingBuffer::evicted_bytes() const { return evicted_bytes_; }

void RingBuffer::clear() {
  chunks_.clear();
  bytes_ = 0;
  evicted_chunks_ = 0;
  evicted_bytes_ = 0;
}

Segmenter::Segmenter(MonotonicMs segment_ms, size_t max_segment_bytes,
                     std::vector<uint8_t> codec_config)
    : segment_ms_(segment_ms),
      max_segment_bytes_(max_segment_bytes),
      codec_config_(std::move(codec_config)) {}

std::optional<FinishedSegment> Segmenter::add(const EncodedChunk& chunk) {
  // A buffer holding no keyframe can never become a decodable segment, and the
  // keyframe-gated roll below could not fire either, so it would grow without
  // bound if the encoder ever stopped emitting keyframes. Drop it (design §5.2).
  if (buffered_bytes_ >= 2 * max_segment_bytes_) {
    size_t first_keyframe = 0;
    while (first_keyframe < pending_.size() && !pending_[first_keyframe].keyframe) {
      ++first_keyframe;
    }
    if (first_keyframe == pending_.size()) discard();
  }

  if (pending_.empty() && !chunk.keyframe) {
    // No segment can begin here, so report none. The chunk is still retained so
    // flush() can emit an event tail that never saw a keyframe, where
    // SegmentInfo::keyframe then records the truth. Production input is already
    // keyframe-led (RingBuffer::since), so only abandoned tails land here.
    pending_.push_back(chunk);
    buffered_bytes_ += chunk.data.size();
    return std::nullopt;
  }

  // Once a keyframe arrives, drop any delta frames buffered ahead of it: a
  // segment must begin decodable, exactly as RingBuffer::since trims the window.
  if (chunk.keyframe) {
    size_t lead = 0;
    while (lead < pending_.size() && !pending_[lead].keyframe) ++lead;
    for (size_t i = 0; i < lead; ++i) buffered_bytes_ -= pending_[i].data.size();
    if (lead > 0) {
      pending_.erase(pending_.begin(),
                     pending_.begin() + static_cast<ptrdiff_t>(lead));
    }
  }

  const bool had_pending = !pending_.empty();
  pending_.push_back(chunk);
  buffered_bytes_ += chunk.data.size();

  // Roll only when the incoming chunk is a keyframe, so every boundary is
  // decodable. Waiting for the next keyframe after a limit is crossed bounds the
  // peak to roughly max_segment_bytes + one GOP, instead of letting a whole
  // segment period's worth of data pile up past the cap.
  const bool time_reached =
      had_pending && chunk.ts_ms - pending_.front().ts_ms >= segment_ms_;
  const bool bytes_reached = had_pending && buffered_bytes_ >= max_segment_bytes_;
  if (!chunk.keyframe || (!time_reached && !bytes_reached)) return std::nullopt;

  // Finish everything buffered before the incoming chunk, which becomes the
  // first chunk of the next segment.
  const bool starts_at_keyframe = pending_.front().keyframe;
  EncodedChunk incoming = std::move(pending_.back());
  pending_.pop_back();
  buffered_bytes_ -= incoming.data.size();

  std::optional<FinishedSegment> finished = emit(starts_at_keyframe);

  buffered_bytes_ += incoming.data.size();
  pending_.push_back(std::move(incoming));
  return finished;
}

std::optional<FinishedSegment> Segmenter::flush() {
  if (pending_.empty()) return std::nullopt;
  return emit(pending_.front().keyframe);
}

void Segmenter::discard() {
  pending_.clear();
  buffered_bytes_ = 0;
}

void Segmenter::reset() {
  discard();
  next_seq_ = 0;
}

std::optional<FinishedSegment> Segmenter::emit(bool starts_at_keyframe) {
  if (pending_.empty()) return std::nullopt;

  size_t payload_bytes = 0;
  for (const EncodedChunk& chunk : pending_) payload_bytes += chunk.data.size();

  // The hub re-muxes without decoding, so a keyframe-led segment must carry the
  // codec config. A segment whose first chunk already embeds it must not get a
  // second copy.
  const bool prepend_config =
      starts_at_keyframe && !codec_config_.empty() &&
      !starts_with(pending_.front().data, codec_config_);

  FinishedSegment finished;
  finished.data.reserve(payload_bytes +
                        (prepend_config ? codec_config_.size() : 0));
  if (prepend_config) {
    finished.data.insert(finished.data.end(), codec_config_.begin(),
                         codec_config_.end());
  }
  for (const EncodedChunk& chunk : pending_) {
    finished.data.insert(finished.data.end(), chunk.data.begin(),
                         chunk.data.end());
  }

  finished.info.seq = next_seq_++;
  char name[32];
  std::snprintf(name, sizeof(name), "seg_%04d.bin", finished.info.seq);
  finished.info.file = name;
  finished.info.bytes = finished.data.size();
  finished.info.sha256 = sha256_hex(finished.data);
  finished.info.keyframe = starts_at_keyframe;
  finished.info.start_ms = pending_.front().ts_ms;
  finished.info.end_ms = pending_.back().ts_ms;
  finished.info.uploaded = false;

  pending_.clear();
  buffered_bytes_ = 0;
  return finished;
}

}  // namespace hv
