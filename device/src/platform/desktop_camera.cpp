#include <algorithm>
#include <chrono>
#include <thread>

#include "platform/backends.h"

namespace hv::internal {

namespace {

/**
 * Synthetic camera for the development machine and the fake-device mode.
 *
 * Emits a deterministic RGB888 pattern so the encoder stub and the recorder can
 * be exercised without hardware, and stamps frames with the injected clock so
 * timestamps share the device's monotonic base.
 */
class DesktopCamera : public ICameraSource {
 public:
  explicit DesktopCamera(const CameraOptions& options) : options_(options) {}

  Result open() override {
    if (options_.width == 0 || options_.height == 0 || options_.fps == 0) {
      last_error_ = "camera options must be non-zero";
      return Result::error(Code::Invalid, last_error_);
    }
    buffer_.assign(static_cast<size_t>(options_.width) * options_.height * 3, 0);
    frame_index_ = 0;
    fallback_origin_ = std::chrono::steady_clock::now();
    opened_ = true;
    return Result::success();
  }

  bool read(Frame& out, int timeout_ms) override {
    if (!opened_) {
      last_error_ = "camera is not open";
      return false;
    }

    const auto interval =
        std::chrono::milliseconds(1000 / std::max<uint32_t>(1, options_.fps));
    if (options_.realtime) {
      const auto due = fallback_origin_ + interval * frame_index_;
      const auto now = std::chrono::steady_clock::now();
      if (due > now) {
        const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(due - now);
        if (timeout_ms >= 0 && wait.count() > timeout_ms) {
          last_error_ = "frame not ready within timeout";
          return false;
        }
        std::this_thread::sleep_for(due - now);
      }
    }

    fill_pattern(frame_index_);
    out.ts_ms = clock_ms();
    out.width = options_.width;
    out.height = options_.height;
    out.format = PixelFormat::Rgb888;
    out.data = buffer_.data();
    out.size = buffer_.size();
    frame_index_++;
    return true;
  }

  void close() override { opened_ = false; }

  const std::string& last_error() const override { return last_error_; }

  std::string name() const override { return "desktop-camera"; }

 private:
  MonotonicMs clock_ms() const {
    if (options_.clock != nullptr) return options_.clock->monotonic_ms();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - fallback_origin_)
        .count();
  }

  void fill_pattern(uint32_t index) {
    // Cheap, deterministic, and different frame to frame: enough for tests.
    for (size_t i = 0; i < buffer_.size(); i += 3) {
      const uint8_t value = static_cast<uint8_t>((index * 7 + i) & 0xff);
      buffer_[i] = value;
      buffer_[i + 1] = static_cast<uint8_t>(value ^ 0x5a);
      buffer_[i + 2] = static_cast<uint8_t>(index & 0xff);
    }
  }

  const CameraOptions options_;
  std::vector<uint8_t> buffer_;
  uint32_t frame_index_ = 0;
  bool opened_ = false;
  std::string last_error_;
  std::chrono::steady_clock::time_point fallback_origin_{};
};

}  // namespace

std::unique_ptr<ICameraSource> make_desktop_camera(const CameraOptions& options) {
  return std::make_unique<DesktopCamera>(options);
}

}  // namespace hv::internal
