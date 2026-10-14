#include "platform/backends.h"

namespace hv {

bool using_desktop_backends() {
#if defined(HV_PLATFORM_V831)
  return false;
#else
  return true;
#endif
}

std::unique_ptr<ICameraSource> create_camera(const CameraOptions& options) {
#if defined(HV_PLATFORM_V831)
  return internal::make_v831_camera(options);
#else
  return internal::make_desktop_camera(options);
#endif
}

std::unique_ptr<IEncoder> create_encoder() {
#if defined(HV_PLATFORM_V831)
  return internal::make_v831_encoder();
#else
  return internal::make_desktop_encoder();
#endif
}

std::unique_ptr<IDetector> create_detector() {
#if defined(HV_PLATFORM_V831)
  return internal::make_v831_detector();
#else
  return internal::make_desktop_detector();
#endif
}

}  // namespace hv
