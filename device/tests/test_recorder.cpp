#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "harness.h"
#include "hv/recorder.h"

using namespace hv;

namespace {

// Python hashlib.sha256 references (computed, not recalled).
const char* const kSha256Empty =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
const char* const kSha256Abc =
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
const char* const kSha256X55 =
    "d5e285683cd4efc02d021a5c62014694958901005d6f71e89e0989fac77e4072";
const char* const kSha256X56 =
    "04c26261370ee7541549d16dee320c723e3fd14671e66a099afe0a377c16888e";
const char* const kSha256X64 =
    "7ce100971f64e7001e8fe5a51973ecdfe1ced42befe7ee8d5fd6219506b5393c";
const char* const kSha256X1000 =
    "44f8354494a5ba03ba1792a8d3e9c534c47a9181980fde7a3f44b06ef2ae7c7f";

std::vector<uint8_t> bytes(size_t count, uint8_t fill) {
  return std::vector<uint8_t>(count, fill);
}

std::vector<uint8_t> test_codec_config() {
  return {0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e};
}

EncodedChunk make_chunk(MonotonicMs ts_ms, bool keyframe, size_t size,
                        uint8_t fill = 0xab) {
  EncodedChunk chunk;
  chunk.ts_ms = ts_ms;
  chunk.keyframe = keyframe;
  chunk.data.assign(size, fill);
  return chunk;
}

bool begins_with(const std::vector<uint8_t>& data,
                 const std::vector<uint8_t>& prefix) {
  return data.size() >= prefix.size() &&
         std::equal(prefix.begin(), prefix.end(), data.begin());
}

// 15 fps: frame i lands at this offset, with a keyframe every 30 frames.
MonotonicMs frame_ts(int index) {
  return static_cast<MonotonicMs>(index) * 1000 / 15;
}

}  // namespace

HV_TEST(recorder_sha256_matches_known_vectors) {
  HV_CHECK_STR_EQ(sha256_hex(std::vector<uint8_t>{}), kSha256Empty);

  const std::string abc = "abc";
  HV_CHECK_STR_EQ(
      sha256_hex(reinterpret_cast<const uint8_t*>(abc.data()), abc.size()),
      kSha256Abc);
}

HV_TEST(recorder_sha256_handles_block_padding_edges) {
  // 55 -> 56 crosses the point where the length no longer fits the first block.
  HV_CHECK_STR_EQ(sha256_hex(bytes(55, 'x')), kSha256X55);
  HV_CHECK_STR_EQ(sha256_hex(bytes(56, 'x')), kSha256X56);
  HV_CHECK_STR_EQ(sha256_hex(bytes(64, 'x')), kSha256X64);
  HV_CHECK_STR_EQ(sha256_hex(bytes(1000, 'x')), kSha256X1000);
}

HV_TEST(ring_buffer_evicts_oldest_to_stay_within_cap) {
  RingBuffer ring(10);
  ring.push(make_chunk(0, true, 4));
  ring.push(make_chunk(1, true, 4));
  HV_CHECK_EQ(ring.bytes(), size_t{8});
  HV_CHECK_EQ(ring.chunks(), size_t{2});
  HV_CHECK_EQ(ring.evicted_chunks(), uint64_t{0});

  ring.push(make_chunk(2, true, 4));
  HV_CHECK_EQ(ring.bytes(), size_t{8});
  HV_CHECK_EQ(ring.chunks(), size_t{2});
  HV_CHECK_EQ(ring.evicted_chunks(), uint64_t{1});
  HV_CHECK_EQ(ring.evicted_bytes(), uint64_t{4});

  ring.push(make_chunk(3, true, 4));
  HV_CHECK_EQ(ring.evicted_chunks(), uint64_t{2});
  HV_CHECK_EQ(ring.evicted_bytes(), uint64_t{8});

  const std::vector<EncodedChunk> kept = ring.since(0);
  HV_CHECK_EQ(kept.size(), size_t{2});
  HV_CHECK_EQ(kept.front().ts_ms, MonotonicMs{2});
  HV_CHECK_EQ(kept.back().ts_ms, MonotonicMs{3});
}

HV_TEST(ring_buffer_keeps_one_oversized_chunk) {
  RingBuffer ring(10);
  ring.push(make_chunk(0, true, 100));
  HV_CHECK_EQ(ring.bytes(), size_t{100});
  HV_CHECK_EQ(ring.chunks(), size_t{1});
  HV_CHECK_EQ(ring.evicted_chunks(), uint64_t{0});

  // The next push makes the oversized chunk evictable again.
  ring.push(make_chunk(1, true, 5));
  HV_CHECK_EQ(ring.bytes(), size_t{5});
  HV_CHECK_EQ(ring.chunks(), size_t{1});
  HV_CHECK_EQ(ring.evicted_chunks(), uint64_t{1});
  HV_CHECK_EQ(ring.evicted_bytes(), uint64_t{100});
}

HV_TEST(ring_buffer_since_filters_by_timestamp) {
  RingBuffer ring(1000);
  ring.push(make_chunk(0, true, 1));
  ring.push(make_chunk(10, false, 1));
  ring.push(make_chunk(20, true, 1));
  ring.push(make_chunk(30, false, 1));
  ring.push(make_chunk(40, true, 1));

  const std::vector<EncodedChunk> from20 = ring.since(15);
  HV_CHECK_EQ(from20.size(), size_t{3});
  HV_CHECK_EQ(from20.front().ts_ms, MonotonicMs{20});
  HV_CHECK_EQ(from20.back().ts_ms, MonotonicMs{40});

  // The first chunk at/after 25 is a delta frame, so it is skipped.
  const std::vector<EncodedChunk> from40 = ring.since(25);
  HV_CHECK_EQ(from40.size(), size_t{1});
  HV_CHECK_EQ(from40.front().ts_ms, MonotonicMs{40});

  HV_CHECK_EQ(ring.since(100).size(), size_t{0});
}

HV_TEST(ring_buffer_since_starts_at_a_keyframe) {
  RingBuffer ring(1000);
  ring.push(make_chunk(0, false, 1));
  ring.push(make_chunk(10, false, 1));
  ring.push(make_chunk(20, true, 1));
  ring.push(make_chunk(30, false, 1));

  const std::vector<EncodedChunk> kept = ring.since(0);
  HV_CHECK_EQ(kept.size(), size_t{2});
  HV_CHECK_TRUE(kept.front().keyframe);
  HV_CHECK_EQ(kept.front().ts_ms, MonotonicMs{20});
}

HV_TEST(ring_buffer_since_is_empty_without_a_keyframe) {
  RingBuffer ring(1000);
  ring.push(make_chunk(0, true, 1));
  ring.push(make_chunk(10, false, 1));
  ring.push(make_chunk(20, false, 1));

  HV_CHECK_EQ(ring.since(5).size(), size_t{0});
}

HV_TEST(ring_buffer_clear_resets_accounting) {
  RingBuffer ring(10);
  ring.push(make_chunk(0, true, 4));
  ring.push(make_chunk(1, true, 4));
  ring.push(make_chunk(2, true, 4));
  HV_CHECK_TRUE(ring.evicted_chunks() > 0);

  ring.clear();
  HV_CHECK_EQ(ring.bytes(), size_t{0});
  HV_CHECK_EQ(ring.chunks(), size_t{0});
  HV_CHECK_EQ(ring.evicted_chunks(), uint64_t{0});
  HV_CHECK_EQ(ring.evicted_bytes(), uint64_t{0});
}

HV_TEST(segmenter_rolls_after_segment_ms_at_the_next_keyframe) {
  const std::vector<uint8_t> config = test_codec_config();
  Segmenter segmenter(60000, 64ull * 1024 * 1024, config);

  const size_t frame_bytes = 100;
  std::optional<FinishedSegment> rolled;
  int rolls = 0;
  for (int i = 0; i < 960; ++i) {
    const auto out =
        segmenter.add(make_chunk(frame_ts(i), i % 30 == 0, frame_bytes));
    if (out) {
      rolled = *out;
      ++rolls;
    }
  }

  // Only the keyframe at t=60000 crosses the 60 s boundary, so exactly one roll.
  HV_CHECK_EQ(rolls, 1);
  HV_CHECK(rolled.has_value());
  HV_CHECK_EQ(rolled->info.seq, 0);
  HV_CHECK_TRUE(rolled->info.keyframe);
  HV_CHECK_EQ(rolled->info.start_ms, MonotonicMs{0});
  HV_CHECK_EQ(rolled->info.end_ms, MonotonicMs{59933});
  HV_CHECK_EQ(rolled->data.size(), size_t{900} * frame_bytes + config.size());

  const auto tail = segmenter.flush();
  HV_CHECK(tail.has_value());
  HV_CHECK_EQ(tail->info.seq, 1);
  HV_CHECK_TRUE(tail->info.keyframe);
  HV_CHECK_EQ(tail->info.start_ms, MonotonicMs{60000});
  HV_CHECK_EQ(tail->info.end_ms, MonotonicMs{63933});
  HV_CHECK_EQ(tail->data.size(), size_t{60} * frame_bytes + config.size());
  HV_CHECK_EQ(segmenter.next_seq(), 2);
  HV_CHECK_FALSE(segmenter.has_pending());
}

HV_TEST(segmenter_rolls_on_the_byte_cap_at_the_next_keyframe) {
  const std::vector<uint8_t> config = test_codec_config();
  Segmenter segmenter(600000, 100, config);

  std::optional<FinishedSegment> rolled;
  for (int i = 0; i < 13; ++i) {
    const auto out =
        segmenter.add(make_chunk(static_cast<MonotonicMs>(i) * 66, i % 4 == 0, 10));
    if (out) rolled = *out;
  }

  // The cap is crossed mid-GOP at frame 12; the roll waits for frame 12 itself.
  HV_CHECK(rolled.has_value());
  HV_CHECK_TRUE(rolled->info.keyframe);
  HV_CHECK_EQ(rolled->info.start_ms, MonotonicMs{0});
  HV_CHECK_EQ(rolled->info.end_ms, MonotonicMs{726});
  HV_CHECK_EQ(rolled->info.bytes,
              static_cast<uint64_t>(12 * 10) + config.size());
  HV_CHECK_TRUE(segmenter.has_pending());
  HV_CHECK_EQ(segmenter.buffered_bytes(), uint64_t{10});
}

HV_TEST(segmenter_prepends_codec_config_only_to_keyframe_led_segments) {
  const std::vector<uint8_t> config = test_codec_config();

  // Keyframe-led: the config comes first, exactly once.
  Segmenter keyed(0, 1ull << 20, config);
  HV_CHECK_FALSE(keyed.add(make_chunk(0, true, 5, 0x11)).has_value());
  const auto first = keyed.add(make_chunk(100, true, 5, 0x22));
  HV_CHECK(first.has_value());
  HV_CHECK_TRUE(first->info.keyframe);
  HV_CHECK_EQ(first->data.size(), config.size() + size_t{5});
  HV_CHECK(begins_with(first->data, config));

  // Delta-led tail from flush(): no config.
  Segmenter tail(60000, 1ull << 20, config);
  HV_CHECK_FALSE(tail.add(make_chunk(0, false, 7, 0x33)).has_value());
  const auto out = tail.flush();
  HV_CHECK(out.has_value());
  HV_CHECK_FALSE(out->info.keyframe);
  HV_CHECK_EQ(out->data.size(), size_t{7});
  HV_CHECK_FALSE(begins_with(out->data, config));
  HV_CHECK_EQ(out->data.front(), uint8_t{0x33});
}

HV_TEST(segmenter_does_not_start_a_segment_on_a_non_keyframe) {
  Segmenter segmenter(60000, 1ull << 20, test_codec_config());
  HV_CHECK_FALSE(segmenter.add(make_chunk(0, false, 12)).has_value());

  // The leading delta frame is skipped and the keyframe opens the segment.
  HV_CHECK_FALSE(segmenter.add(make_chunk(100, true, 12)).has_value());
  const auto out = segmenter.flush();
  HV_CHECK(out.has_value());
  HV_CHECK_TRUE(out->info.keyframe);
  HV_CHECK_EQ(out->info.start_ms, MonotonicMs{100});
}

HV_TEST(segmenter_drops_an_undecodable_pile_instead_of_growing) {
  Segmenter segmenter(60000, 100, test_codec_config());
  // Delta frames only: no decodable segment can be built, so the buffer must not
  // grow past the hard cap just because no keyframe ever arrives.
  for (int i = 0; i < 12; ++i) {
    segmenter.add(make_chunk(i * 10, false, 20, static_cast<uint8_t>(i)));
  }
  HV_CHECK(segmenter.buffered_bytes() < size_t{2} * 100 + 20);

  // A keyframe afterwards still opens a normal, keyframe-led segment.
  HV_CHECK_FALSE(segmenter.add(make_chunk(200, true, 20, 0x77)).has_value());
  const auto out = segmenter.flush();
  HV_CHECK(out.has_value());
  HV_CHECK_TRUE(out->info.keyframe);
  HV_CHECK_EQ(out->info.start_ms, MonotonicMs{200});
}

HV_TEST(segmenter_flush_on_empty_is_nullopt) {
  Segmenter segmenter(60000, 1ull << 20, test_codec_config());
  HV_CHECK_FALSE(segmenter.flush().has_value());
  HV_CHECK_FALSE(segmenter.has_pending());
  HV_CHECK_EQ(segmenter.buffered_bytes(), uint64_t{0});
  HV_CHECK_EQ(segmenter.next_seq(), 0);
}

HV_TEST(segmenter_seq_increments_and_reset_restarts) {
  Segmenter segmenter(0, 1ull << 20, test_codec_config());

  std::vector<int> seqs;
  for (int i = 0; i < 3; ++i) {
    const auto out = segmenter.add(make_chunk(
        static_cast<MonotonicMs>(i) * 100, true, 8, static_cast<uint8_t>(i)));
    if (out) seqs.push_back(out->info.seq);
  }
  HV_CHECK_EQ(seqs.size(), size_t{2});
  HV_CHECK_EQ(seqs[0], 0);
  HV_CHECK_EQ(seqs[1], 1);
  HV_CHECK_EQ(segmenter.next_seq(), 2);

  segmenter.reset();
  HV_CHECK_EQ(segmenter.next_seq(), 0);
  HV_CHECK_FALSE(segmenter.has_pending());

  HV_CHECK_FALSE(segmenter.add(make_chunk(0, true, 8, 0x01)).has_value());
  const auto rolled = segmenter.add(make_chunk(100, true, 8, 0x02));
  HV_CHECK(rolled.has_value());
  HV_CHECK_EQ(rolled->info.seq, 0);
  HV_CHECK_STR_EQ(rolled->info.file, "seg_0000.bin");
}

HV_TEST(segmenter_sha256_describes_the_payload) {
  const std::vector<uint8_t> config = test_codec_config();
  Segmenter a(0, 1ull << 20, config);
  Segmenter b(0, 1ull << 20, config);

  a.add(make_chunk(0, true, 32, 0x5a));
  b.add(make_chunk(0, true, 32, 0x5a));
  const auto seg_a = a.add(make_chunk(100, true, 16, 0x5b));
  const auto seg_b = b.add(make_chunk(100, true, 16, 0x5b));

  HV_CHECK(seg_a.has_value());
  HV_CHECK(seg_b.has_value());
  HV_CHECK_EQ(seg_a->info.bytes, static_cast<uint64_t>(seg_a->data.size()));
  HV_CHECK_EQ(seg_a->data.size(), config.size() + size_t{32});
  HV_CHECK_STR_EQ(seg_a->info.sha256, sha256_hex(seg_a->data));
  // Identical input must hash identically.
  HV_CHECK_STR_EQ(seg_a->info.sha256, seg_b->info.sha256);
  HV_CHECK_STR_EQ(seg_a->info.file, "seg_0000.bin");
}

HV_TEST(segmenter_discard_drops_without_emitting) {
  Segmenter segmenter(60000, 1ull << 20, test_codec_config());
  segmenter.add(make_chunk(0, true, 16));
  HV_CHECK_TRUE(segmenter.has_pending());

  segmenter.discard();
  HV_CHECK_FALSE(segmenter.has_pending());
  HV_CHECK_EQ(segmenter.buffered_bytes(), uint64_t{0});
  HV_CHECK_FALSE(segmenter.flush().has_value());
}
