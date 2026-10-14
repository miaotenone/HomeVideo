#include <algorithm>

#include "platform/backends.h"

namespace hv::internal {

namespace {

/**
 * Scripted detector stub.
 *
 * Reports only its own label, and only on the run indices the configuration
 * scripts (see DetectorConfig::simulation). Used by unit tests and by the
 * `--fake-device` mode so a full event cycle can be driven without a model.
 */
class DesktopDetector : public IDetector {
 public:
  Result load(const DetectorConfig& config) override {
    if (config.input_width == 0 || config.input_height == 0) {
      last_error_ = "detector input size must be > 0";
      return Result::error(Code::Invalid, last_error_);
    }
    config_ = config;
    run_count_ = 0;
    loaded_ = true;
    return Result::success();
  }

  bool run(const Frame&, std::vector<Detection>& out) override {
    if (!loaded_) {
      last_error_ = "detector is not loaded";
      return false;
    }
    run_count_++;
    out.clear();
    for (const auto& script : config_.simulation) {
      const uint32_t first = std::max<uint32_t>(1, script.at_run);
      const uint32_t runs = std::max<uint32_t>(1, script.runs);
      if (run_count_ < first || run_count_ >= first + runs) continue;
      if (script.confidence < config_.conf_threshold) continue;

      Detection detection;
      detection.label = config_.label;
      detection.confidence = script.confidence;
      detection.bbox = script.bbox;
      detection.zone = script.zone;
      detection.ts_ms = 0;  // the scheduler stamps it with the device clock
      out.push_back(std::move(detection));
    }
    return true;
  }

  const std::string& model_version() const override { return config_.model_version; }

  void unload() override { loaded_ = false; }

  const std::string& last_error() const override { return last_error_; }

  std::string name() const override { return "desktop-detector"; }

 private:
  DetectorConfig config_;
  uint32_t run_count_ = 0;
  bool loaded_ = false;
  std::string last_error_;
};

}  // namespace

std::unique_ptr<IDetector> make_desktop_detector() {
  return std::make_unique<DesktopDetector>();
}

}  // namespace hv::internal
