#include "platform/backends.h"

namespace hv::internal {

namespace {

/**
 * V831 hardware H.264 encoder — not implemented yet.
 *
 * Blocked on phase A: confirming that the MPP encoder is callable from the
 * flashed image, and whether the headers come from the image or from the Tina
 * SDK (docs/homevideo-device-design.md §12).
 *
 * TODO(phase-A): implement over eyesee-mpp (or the Tina SDK build path) and
 * fill codec_config() with the real SPS/PPS.
 */
class V831Encoder : public IEncoder {
 public:
  Result open(const EncoderConfig&) override { return unavailable(); }
  bool encode(const Frame&, EncodedChunk&) override { return false; }

  const std::vector<uint8_t>& codec_config() const override { return config_; }

  void request_keyframe() override {}
  void close() override {}

  const std::string& last_error() const override { return error_; }

  std::string name() const override { return "v831-encoder"; }

 private:
  Result unavailable() {
    error_ =
        "v831 encoder backend is not implemented; see "
        "docs/homevideo-device-design.md §12 (phase A)";
    return Result::error(Code::Internal, error_);
  }

  std::vector<uint8_t> config_;
  std::string error_;
};

}  // namespace

std::unique_ptr<IEncoder> make_v831_encoder() {
  return std::make_unique<V831Encoder>();
}

}  // namespace hv::internal
