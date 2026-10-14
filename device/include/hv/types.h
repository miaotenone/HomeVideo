#pragma once

// Types shared by every device module. Deliberately dependency-free so this
// header can be included anywhere (including the platform backends).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hv {

/** Milliseconds from the process start clock. Never wall-clock derived. */
using MonotonicMs = int64_t;

enum class PixelFormat { Yuv420, Rgb888 };

enum class EventType { PersonIntrusion, FireSuspected, SmokeSuspected };

enum class ArmMode { Home, Away, Unknown };

/** Wire spellings are fixed by the hub protocol; changes break interop. */
inline const char* to_wire(EventType type) {
  switch (type) {
    case EventType::PersonIntrusion:
      return "person_intrusion";
    case EventType::FireSuspected:
      return "fire_suspected";
    case EventType::SmokeSuspected:
      return "smoke_suspected";
  }
  return "unknown";
}

inline const char* to_wire(ArmMode mode) {
  switch (mode) {
    case ArmMode::Home:
      return "home";
    case ArmMode::Away:
      return "away";
    case ArmMode::Unknown:
      return "unknown";
  }
  return "unknown";
}

/** Human-readable spelling for logs and test failure output. */
inline const char* to_string(EventType type) { return to_wire(type); }
inline const char* to_string(ArmMode mode) { return to_wire(mode); }

inline std::optional<EventType> event_type_from_wire(const std::string& text) {
  if (text == "person_intrusion") return EventType::PersonIntrusion;
  if (text == "fire_suspected") return EventType::FireSuspected;
  if (text == "smoke_suspected") return EventType::SmokeSuspected;
  return std::nullopt;
}

inline std::optional<ArmMode> arm_mode_from_wire(const std::string& text) {
  if (text == "home") return ArmMode::Home;
  if (text == "away") return ArmMode::Away;
  if (text == "unknown") return ArmMode::Unknown;
  return std::nullopt;
}

/** Fixed slot per label, so per-label state can live in a std::array of 3. */
inline size_t index_of(EventType type) {
  switch (type) {
    case EventType::PersonIntrusion:
      return 0;
    case EventType::FireSuspected:
      return 1;
    case EventType::SmokeSuspected:
      return 2;
  }
  return 0;
}

inline constexpr size_t kEventTypeCount = 3;

struct BBox {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
};

/**
 * One captured frame.
 *
 * `data` is owned by the camera source and only guaranteed valid until the next
 * read(); consumers that need to keep pixels must copy them.
 */
struct Frame {
  MonotonicMs ts_ms = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  PixelFormat format = PixelFormat::Yuv420;
  const uint8_t* data = nullptr;
  size_t size = 0;

  bool valid() const { return data != nullptr && size > 0; }
};

/** One encoded unit. For H.264 this is one access unit (Annex-B). */
struct EncodedChunk {
  MonotonicMs ts_ms = 0;
  bool keyframe = false;
  std::vector<uint8_t> data;
};

struct Detection {
  EventType label = EventType::PersonIntrusion;
  float confidence = 0.0f;
  std::optional<BBox> bbox;
  std::optional<std::string> zone;
  MonotonicMs ts_ms = 0;
};

struct EncoderConfig {
  std::string codec = "h264";
  uint32_t width = 1280;
  uint32_t height = 720;
  uint32_t fps = 15;
  uint32_t bitrate_bps = 1500000;
  uint32_t gop_ms = 2000;
};

/**
 * Deterministic scripting for the simulated detector.
 *
 * Only the desktop / fake-device backend reads this; real backends ignore it.
 * `at_run` is the 1-based run() index that first reports a detection, which
 * repeats for `runs` consecutive runs.
 */
struct SimulatedDetection {
  uint32_t at_run = 1;
  uint32_t runs = 1;
  float confidence = 0.9f;
  std::optional<BBox> bbox;
  std::optional<std::string> zone;
};

/** One detector instance per event label (person / fire / smoke). */
struct DetectorConfig {
  EventType label = EventType::PersonIntrusion;
  std::string model_path;
  uint32_t input_width = 224;
  uint32_t input_height = 224;
  float conf_threshold = 0.5f;
  std::string model_version = "unset";
  std::vector<SimulatedDetection> simulation;
};

}  // namespace hv
