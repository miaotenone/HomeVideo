#include <algorithm>
#include <cstdint>

#include "platform/backends.h"
#include "platform/synthetic_snapshot.h"

namespace hv::internal {

namespace {

/**
 * Encoder stub.
 *
 * Emits one deterministic chunk per frame with a keyframe every `gop_ms`, and
 * exposes plausible H.264 parameter sets so the segmenter can prepend them.
 * It deliberately does NOT prepend them itself: the segmenter owns
 * "each keyframe-aligned segment is self-describing" (design §5.2).
 */
class DesktopEncoder : public IEncoder {
 public:
  Result open(const EncoderConfig& config) override {
    if (config.fps == 0 || config.gop_ms == 0) {
      last_error_ = "encoder config must have fps and gop_ms > 0";
      return Result::error(Code::Invalid, last_error_);
    }
    config_ = config;
    // Annex-B style parameter sets: start code + SPS, start code + PPS.
    codec_config_ = {0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e,
                     0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x3c, 0x80};
    frame_index_ = 0;
    force_keyframe_ = false;
    opened_ = true;
    return Result::success();
  }

  bool encode(const Frame& in, EncodedChunk& out) override {
    if (!opened_) {
      last_error_ = "encoder is not open";
      return false;
    }
    const uint32_t gop_frames = std::max<uint32_t>(
        1, (config_.gop_ms * config_.fps) / 1000);
    const bool keyframe = force_keyframe_ || (frame_index_ % gop_frames) == 0;
    force_keyframe_ = false;

    out.ts_ms = in.ts_ms;
    out.keyframe = keyframe;
    out.data.clear();
    out.data.reserve(32);
    for (int i = 0; i < 16; ++i) {
      out.data.push_back(
          static_cast<uint8_t>((frame_index_ * 31 + i * 7 + (keyframe ? 1 : 0)) & 0xff));
    }
    frame_index_++;
    return true;
  }

  const std::vector<uint8_t>& codec_config() const override { return codec_config_; }

  bool encode_snapshot(const Frame& in, std::vector<uint8_t>& jpeg_out) override {
    if (!opened_) {
      last_error_ = "encoder is not open";
      return false;
    }
    // A constant still: the synthetic camera's pixels carry no information a
    // viewer would want, and the hub never decodes the blob, so there is
    // nothing to gain from a real JPEG encoder here (see synthetic_snapshot.h).
    (void)in;
    jpeg_out = synthetic_snapshot_jpeg();
    return true;
  }

  void request_keyframe() override { force_keyframe_ = true; }

  void close() override { opened_ = false; }

  const std::string& last_error() const override { return last_error_; }

  std::string name() const override { return "desktop-encoder"; }

 private:
  EncoderConfig config_;
  std::vector<uint8_t> codec_config_;
  uint32_t frame_index_ = 0;
  bool force_keyframe_ = false;
  bool opened_ = false;
  std::string last_error_;
};

}  // namespace

std::unique_ptr<IEncoder> make_desktop_encoder() {
  return std::make_unique<DesktopEncoder>();
}

}  // namespace hv::internal
