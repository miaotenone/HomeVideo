#include <memory>
#include <string>

#include "harness.h"
#include "hv/clock.h"
#include "hv/platform.h"
#include "platform/backends.h"

using namespace hv;

namespace {

CameraOptions test_camera_options(const IClock* clock) {
  CameraOptions options;
  options.width = 64;
  options.height = 48;
  options.fps = 15;
  options.realtime = false;  // no sleeping: tests stay fast and deterministic
  options.clock = clock;
  return options;
}

DetectorConfig scripted_detector(EventType label, uint32_t at_run, uint32_t runs) {
  DetectorConfig config;
  config.label = label;
  config.model_version = std::string("stub-") + to_wire(label);
  SimulatedDetection script;
  script.at_run = at_run;
  script.runs = runs;
  script.confidence = 0.9f;
  config.simulation.push_back(script);
  return config;
}

}  // namespace

HV_TEST(wire_spellings_round_trip) {
  HV_CHECK_STR_EQ(to_wire(EventType::PersonIntrusion), "person_intrusion");
  HV_CHECK_STR_EQ(to_wire(EventType::FireSuspected), "fire_suspected");
  HV_CHECK_STR_EQ(to_wire(EventType::SmokeSuspected), "smoke_suspected");
  HV_CHECK_STR_EQ(to_wire(ArmMode::Home), "home");
  HV_CHECK_STR_EQ(to_wire(ArmMode::Away), "away");
  HV_CHECK_STR_EQ(to_wire(ArmMode::Unknown), "unknown");

  const EventType types[] = {EventType::PersonIntrusion, EventType::FireSuspected,
                             EventType::SmokeSuspected};
  for (const EventType type : types) {
    const auto parsed = event_type_from_wire(to_wire(type));
    HV_CHECK(parsed.has_value());
    HV_CHECK_EQ(parsed.value(), type);
  }
  const ArmMode modes[] = {ArmMode::Home, ArmMode::Away, ArmMode::Unknown};
  for (const ArmMode mode : modes) {
    const auto parsed = arm_mode_from_wire(to_wire(mode));
    HV_CHECK(parsed.has_value());
    HV_CHECK_EQ(parsed.value(), mode);
  }

  HV_CHECK_FALSE(event_type_from_wire("person").has_value());
  HV_CHECK_FALSE(event_type_from_wire("").has_value());
  HV_CHECK_FALSE(arm_mode_from_wire("armed").has_value());
}

HV_TEST(camera_stamps_frames_with_the_injected_clock) {
  FakeClock clock;
  auto camera = create_camera(test_camera_options(&clock));
  HV_CHECK(camera->open().ok());

  MonotonicMs previous = -1;
  for (int i = 0; i < 5; ++i) {
    clock.advance_ms(66);
    Frame frame;
    HV_CHECK(camera->read(frame, 0));
    HV_CHECK_NEAR(static_cast<double>(frame.ts_ms),
                  static_cast<double>(clock.monotonic_ms()), 0.001);
    HV_CHECK(frame.ts_ms > previous);
    previous = frame.ts_ms;
    HV_CHECK_EQ(frame.width, uint32_t{64});
    HV_CHECK_EQ(frame.height, uint32_t{48});
    HV_CHECK_EQ(frame.format, PixelFormat::Rgb888);
    HV_CHECK_TRUE(frame.valid());
    HV_CHECK_EQ(frame.size, size_t{64 * 48 * 3});
  }
  camera->close();
}

HV_TEST(camera_rejects_invalid_options) {
  CameraOptions options = test_camera_options(nullptr);
  options.fps = 0;
  auto camera = create_camera(options);
  const Result result = camera->open();
  HV_CHECK_FALSE(result.ok());
  HV_CHECK_EQ(result.code(), Code::Invalid);
  Frame frame;
  HV_CHECK_FALSE(camera->read(frame, 0));  // never opened
}

HV_TEST(encoder_emits_keyframes_on_the_gop_cadence) {
  auto encoder = create_encoder();
  EncoderConfig config;
  config.fps = 15;
  config.gop_ms = 2000;  // 30 frames
  HV_CHECK(encoder->open(config).ok());
  HV_CHECK_FALSE(encoder->codec_config().empty());

  Frame frame;
  frame.ts_ms = 0;
  int keyframes = 0;
  for (int i = 0; i < 60; ++i) {
    frame.ts_ms = i * 66;
    EncodedChunk chunk;
    HV_CHECK(encoder->encode(frame, chunk));
    HV_CHECK_EQ(chunk.ts_ms, frame.ts_ms);
    if (chunk.keyframe) {
      keyframes++;
      HV_CHECK(i == 0 || i == 30);
    }
    HV_CHECK_FALSE(chunk.data.empty());
  }
  HV_CHECK_EQ(keyframes, 2);
}

HV_TEST(encoder_request_keyframe_makes_the_next_frame_a_keyframe) {
  auto encoder = create_encoder();
  EncoderConfig config;
  config.fps = 15;
  config.gop_ms = 2000;
  HV_CHECK(encoder->open(config).ok());

  Frame frame;
  EncodedChunk chunk;
  HV_CHECK(encoder->encode(frame, chunk));
  HV_CHECK_TRUE(chunk.keyframe);
  HV_CHECK(encoder->encode(frame, chunk));
  HV_CHECK_FALSE(chunk.keyframe);

  encoder->request_keyframe();
  HV_CHECK(encoder->encode(frame, chunk));
  HV_CHECK_TRUE(chunk.keyframe);
}

HV_TEST(detector_reports_only_its_own_label_on_the_scripted_runs) {
  auto detector = create_detector();
  HV_CHECK(detector->load(scripted_detector(EventType::FireSuspected, 2, 2)).ok());
  HV_CHECK_STR_EQ(detector->model_version(), "stub-fire_suspected");

  Frame frame;
  frame.ts_ms = 0;
  std::vector<Detection> detections;

  HV_CHECK(detector->run(frame, detections));
  HV_CHECK_EQ(detections.size(), size_t{0});
  HV_CHECK(detector->run(frame, detections));
  HV_CHECK_EQ(detections.size(), size_t{1});
  HV_CHECK_EQ(detections[0].label, EventType::FireSuspected);
  HV_CHECK(detector->run(frame, detections));
  HV_CHECK_EQ(detections.size(), size_t{1});
  HV_CHECK(detector->run(frame, detections));
  HV_CHECK_EQ(detections.size(), size_t{0});
}

HV_TEST(detector_applies_the_confidence_threshold) {
  DetectorConfig config = scripted_detector(EventType::PersonIntrusion, 1, 1);
  config.conf_threshold = 0.95f;  // script reports 0.9
  auto detector = create_detector();
  HV_CHECK(detector->load(config).ok());
  Frame frame;
  std::vector<Detection> detections;
  HV_CHECK(detector->run(frame, detections));
  HV_CHECK_EQ(detections.size(), size_t{0});
}

HV_TEST(factory_selects_the_build_target_backends) {
#if defined(HV_PLATFORM_V831)
  HV_CHECK_FALSE(using_desktop_backends());
#else
  HV_CHECK_TRUE(using_desktop_backends());
  HV_CHECK_STR_EQ(create_camera(test_camera_options(nullptr))->name(), "desktop-camera");
  HV_CHECK_STR_EQ(create_encoder()->name(), "desktop-encoder");
  HV_CHECK_STR_EQ(create_detector()->name(), "desktop-detector");
#endif
}

HV_TEST(v831_backends_refuse_to_start_until_phase_a) {
  const Result camera = internal::make_v831_camera(test_camera_options(nullptr))->open();
  HV_CHECK_FALSE(camera.ok());
  HV_CHECK_EQ(camera.code(), Code::Internal);

  const Result encoder = internal::make_v831_encoder()->open(EncoderConfig{});
  HV_CHECK_FALSE(encoder.ok());
  HV_CHECK_EQ(encoder.code(), Code::Internal);

  const Result detector = internal::make_v831_detector()->load(DetectorConfig{});
  HV_CHECK_FALSE(detector.ok());
  HV_CHECK_EQ(detector.code(), Code::Internal);
}
