#pragma once

// Backend constructors, one per build target. Kept internal so the public
// header only exposes the generic factories in hv/platform.h.

#include <memory>

#include "hv/platform.h"

namespace hv::internal {

std::unique_ptr<ICameraSource> make_desktop_camera(const CameraOptions& options);
std::unique_ptr<IEncoder> make_desktop_encoder();
std::unique_ptr<IDetector> make_desktop_detector();

std::unique_ptr<ICameraSource> make_v831_camera(const CameraOptions& options);
std::unique_ptr<IEncoder> make_v831_encoder();
std::unique_ptr<IDetector> make_v831_detector();

}  // namespace hv::internal
