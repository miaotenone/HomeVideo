#pragma once

// Upload scheduling: ordering, rate limiting and bounded backoff.
//
// Design §7.1: metadata and snapshot go first, segments follow, close last;
// retries use a capped backoff so a network outage never spins the CPU; content
// is only considered sent once the hub has durably accepted it.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "hv/clock.h"
#include "hv/hub_client.h"
#include "hv/spool.h"
#include "hv/status.h"

namespace hv {

struct BackoffPolicy {
  int64_t base_ms = 500;
  int64_t max_ms = 60000;
  int max_attempts = 8;
  /** Multiplicative jitter in [0, jitter); 0 makes delays deterministic. */
  double jitter = 0.25;
};

/** Deterministic for a given `rng` value, so tests can pin the sequence. */
int64_t backoff_delay_ms(const BackoffPolicy& policy, int attempt, uint64_t rng);

struct UploadStats {
  uint64_t meta_ok = 0;
  uint64_t snapshot_ok = 0;
  uint64_t segment_ok = 0;
  uint64_t close_ok = 0;
  uint64_t retries = 0;
  uint64_t duplicates = 0;
  uint64_t conflicts = 0;
  uint64_t permanent_failures = 0;
  uint64_t bytes_sent = 0;
  uint64_t events_cleaned = 0;
};

struct UploaderLimits {
  BackoffPolicy backoff;
  uint64_t rate_bytes_per_s = 256ull * 1024;
  /** Largest number of pending items considered in a single run_for() pass. */
  size_t batch = 16;
};

class Uploader {
 public:
  Uploader(Spool& spool, HubClient& client, IClock& clock, UploaderLimits limits);
  /** Out of line: Impl is incomplete here (see the same note on Pipeline). */
  ~Uploader();

  Uploader(const Uploader&) = delete;
  Uploader& operator=(const Uploader&) = delete;

  /** Works the pending queue until the budget is spent or nothing is left. */
  void run_for(MonotonicMs budget_ms);

  /** Works at most `max_items` items; used by --once and by tests. */
  size_t run_items(size_t max_items);

  void request_stop();
  UploadStats stats() const;

  /** Attempt counter for one item, for diagnostics and tests. */
  int attempts_for(const std::string& event_id, int seq, PendingItem::Kind kind) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace hv
