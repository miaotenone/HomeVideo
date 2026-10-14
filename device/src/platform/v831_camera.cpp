#include "platform/backends.h"

namespace hv::internal {

namespace {

/**
 * V831 camera backend — not implemented yet.
 *
 * Blocked on the phase-A measurements (docs/homevideo-device-design.md §12):
 * whether the image can hand one capture stream to both the encoder and the
 * detector, or whether the backend must scale/decimate internally.
 *
 * TODO(phase-A): implement with MPP/libmaix once the image is flashed and the
 * capture interface is known.
 */
class V831Camera : public ICameraSource {
 public:
  explicit V831Camera(const CameraOptions&) {}

  Result open() override { return unavailable(); }
  bool read(Frame&, int) override { return false; }
  void close() override {}

  const std::string& last_error() const override { return error_; }

  std::string name() const override { return "v831-camera"; }

 private:
  Result unavailable() {
    error_ =
        "v831 camera backend is not implemented; see "
        "docs/homevideo-device-design.md §12 (phase A)";
    return Result::error(Code::Internal, error_);
  }

  std::string error_;
};

}  // namespace

std::unique_ptr<ICameraSource> make_v831_camera(const CameraOptions& options) {
  return std::make_unique<V831Camera>(options);
}

}  // namespace hv::internal
