#pragma once

// Top-level wiring: config -> spool -> pipeline -> events -> upload -> health.
//
// `run` owns the main loop and is the only place threads are joined, which keeps
// shutdown ordering (flush the pipeline, persist state, then exit) in one place.

#include <filesystem>
#include <memory>
#include <string>

#include "hv/clock.h"
#include "hv/config.h"
#include "hv/status.h"

namespace hv {

struct RunOptions {
  std::filesystem::path config_path;
  /** Run one short cycle then exit 0: used by tools/e2e_with_hub.sh. */
  bool once = false;
  /** Synthetic camera + stub encoder + scripted detector, no hardware. */
  bool fake_device = false;
  bool verbose = false;
  /** Wall-clock budget for --once, in milliseconds. */
  int64_t once_budget_ms = 8000;
};

class App {
 public:
  App(DeviceConfig config, std::unique_ptr<IClock> clock);
  ~App();

  App(const App&) = delete;
  App& operator=(const App&) = delete;

  int run(const RunOptions& options);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace hv
