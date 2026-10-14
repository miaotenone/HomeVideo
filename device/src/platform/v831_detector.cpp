#include "platform/backends.h"

namespace hv::internal {

namespace {

/**
 * V831 NPU detector — not implemented yet.
 *
 * Blocked on phase A: model conversion tooling, the resulting model format, and
 * how much memory three resident models actually cost
 * (docs/homevideo-device-design.md §12).
 *
 * TODO(phase-A): implement over libmaix's nn runtime once a converted model
 * runs on the board.
 */
class V831Detector : public IDetector {
 public:
  Result load(const DetectorConfig&) override { return unavailable(); }
  bool run(const Frame&, std::vector<Detection>&) override { return false; }

  const std::string& model_version() const override { return model_version_; }

  void unload() override {}

  const std::string& last_error() const override { return error_; }

  std::string name() const override { return "v831-detector"; }

 private:
  Result unavailable() {
    error_ =
        "v831 detector backend is not implemented; see "
        "docs/homevideo-device-design.md §12 (phase A)";
    return Result::error(Code::Internal, error_);
  }

  std::string model_version_ = "unset";
  std::string error_;
};

}  // namespace

std::unique_ptr<IDetector> make_v831_detector() {
  return std::make_unique<V831Detector>();
}

}  // namespace hv::internal
