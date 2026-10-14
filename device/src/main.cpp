// Command-line entry point for the HomeVideo device.
//
// The CLI is frozen by tools/e2e_with_hub.sh:
//   hv_device [--config PATH] [--fake-device] [--once] [--once-budget-ms N]
//             [--verbose] [--help]
//
// Exit codes: 0 success, 2 usage error, 1 setup or runtime failure.

#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <string>

#include "hv/app.h"
#include "hv/clock.h"
#include "hv/config.h"
#include "hv/log.h"
#include "hv/platform.h"

namespace {

void print_usage(std::ostream& out) {
  out << "Usage: hv_device [--config PATH] [--fake-device] [--once]"
         " [--once-budget-ms N] [--verbose] [--help]\n"
      << "\n"
      << "  --config PATH         device configuration JSON file\n"
      << "  --fake-device         synthetic camera, stub encoder, scripted"
         " detectors (desktop builds only)\n"
      << "  --once                run one short cycle, upload, then exit\n"
      << "  --once-budget-ms N    wall-clock budget for --once in ms\n"
      << "  --verbose             debug logging\n"
      << "  --help                print this message and exit\n";
}

bool parse_int(const char* text, int64_t& out) {
  if (text == nullptr || *text == '\0') return false;
  char* end = nullptr;
  const long long value = std::strtoll(text, &end, 10);
  if (end == text || *end != '\0') return false;
  out = static_cast<int64_t>(value);
  return true;
}

// False is a usage error (exit 2). `help` is set for --help.
bool parse_args(int argc, char** argv, hv::RunOptions& options, bool& help) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--config") {
      if (i + 1 >= argc) return false;
      options.config_path = argv[++i];
    } else if (arg == "--fake-device") {
      options.fake_device = true;
    } else if (arg == "--once") {
      options.once = true;
    } else if (arg == "--once-budget-ms") {
      if (i + 1 >= argc) return false;
      int64_t value = 0;
      if (!parse_int(argv[++i], value) || value < 0) return false;
      options.once_budget_ms = value;
    } else if (arg == "--verbose") {
      options.verbose = true;
    } else if (arg == "--help") {
      help = true;
    } else {
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  hv::RunOptions options;
  bool help = false;
  if (!parse_args(argc, argv, options, help)) {
    std::cerr << "hv_device: invalid arguments (try --help)\n";
    return 2;
  }
  if (help) {
    print_usage(std::cout);
    return 0;
  }

  try {
    hv::DeviceConfig config;
    // The explicit overload reads the real process environment, so HOMEVIDEO_*
    // overrides file values (env > file > defaults).
    const hv::Result loaded = hv::load_config(options.config_path, config);
    if (!loaded.ok()) {
      HV_ERROR("config: " << loaded.describe());
      return 1;
    }

    if (options.fake_device) {
      if (!hv::using_desktop_backends()) {
        HV_ERROR("--fake-device is only available on a desktop build");
        return 1;
      }
      HV_INFO("fake device: synthetic camera, stub encoder, scripted detectors");
    }

    hv::App app(std::move(config), std::make_unique<hv::SystemClock>());
    return app.run(options);
  } catch (const std::exception& error) {
    HV_ERROR("fatal: " << error.what());
    return 1;
  } catch (...) {
    HV_ERROR("fatal: unknown exception");
    return 1;
  }
}
