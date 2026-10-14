#pragma once

// The hardware seam.
//
// These three interfaces are the only place the device touches hardware. Every
// other module works against them, so the whole pipeline can be built, run and
// tested on a development machine with the desktop backends.
//
// Contract notes that backends must honour (see docs/homevideo-device-design.md §4):
//   - ICameraSource::read may be shared by the encode and detect paths. If the
//     hardware only exposes one stream, the backend does the scaling/decimation
//     internally; callers never see two cameras.
//   - IEncoder::codec_config() must be available before the first keyframe; the
//     segmenter prepends it so every keyframe-aligned segment is self-describing
//     (the hub may only remux, never decode).
//   - IDetector::run must not take ownership of the frame.

#include <memory>
#include <string>
#include <vector>

#include "hv/clock.h"
#include "hv/status.h"
#include "hv/types.h"

namespace hv {

class ICameraSource {
 public:
  virtual ~ICameraSource() = default;
  virtual Result open() = 0;
  /** False on timeout or error; callers treat repeated failures as a health signal. */
  virtual bool read(Frame& out, int timeout_ms) = 0;
  virtual void close() = 0;
  virtual const std::string& last_error() const = 0;
  virtual std::string name() const = 0;
};

class IEncoder {
 public:
  virtual ~IEncoder() = default;
  virtual Result open(const EncoderConfig& config) = 0;
  /** Produces at most one chunk per frame; false means "nothing to emit yet". */
  virtual bool encode(const Frame& in, EncodedChunk& out) = 0;
  virtual const std::vector<uint8_t>& codec_config() const = 0;
  virtual void request_keyframe() = 0;
  virtual void close() = 0;
  virtual const std::string& last_error() const = 0;
  virtual std::string name() const = 0;
};

class IDetector {
 public:
  virtual ~IDetector() = default;
  virtual Result load(const DetectorConfig& config) = 0;
  virtual bool run(const Frame& in, std::vector<Detection>& out) = 0;
  virtual const std::string& model_version() const = 0;
  virtual void unload() = 0;
  virtual const std::string& last_error() const = 0;
  virtual std::string name() const = 0;
};

struct CameraOptions {
  uint32_t width = 1280;
  uint32_t height = 720;
  uint32_t fps = 15;
  /** Desktop only: sleep between frames to emulate real timing. */
  bool realtime = true;
  /** Desktop only: deterministic synthetic content seed. */
  uint32_t seed = 1;
  /**
   * Frame timestamps must live on the same monotonic base as the rest of the
   * device, so the camera stamps frames with this clock. Null picks an internal
   * steady clock (fine for isolated tests, not for the pipeline).
   */
  const IClock* clock = nullptr;
};

/** Backends selected at build time; see src/platform/factory.cpp. */
std::unique_ptr<ICameraSource> create_camera(const CameraOptions& options);
std::unique_ptr<IEncoder> create_encoder();
std::unique_ptr<IDetector> create_detector();

/** True when the binary was built against the desktop backends. */
bool using_desktop_backends();

}  // namespace hv
