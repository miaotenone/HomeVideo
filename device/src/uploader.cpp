#include "hv/uploader.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace hv {

namespace {

namespace fs = std::filesystem;

// Control frames (metadata, close) carry no media to measure, but they still
// occupy the link; charging them a small fixed size keeps them from being free
// for rate-limiting purposes.
constexpr uint64_t kControlBytes = 1024;

// splitmix64 finaliser. The jitter must be a pure function of the caller's rng
// (design §7.1 "退避：指数 + 抖动"): seeding from std::random_device would make a
// retry schedule non-reproducible and therefore untestable.
uint64_t mix64(uint64_t x) {
  x += 0x9e3779b97f4a7c15ull;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
  return x ^ (x >> 31);
}

// FNV-1a. Stable for a given event id so an item's jittered backoff does not
// change between runs or between restarts.
uint64_t hash_text(const std::string& text) {
  uint64_t h = 1469598103934665603ull;
  for (const unsigned char c : text) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

// Identity of one uploadable item; mirrors attempts_for()'s (event, seq, kind).
struct ItemKey {
  std::string event_id;
  int seq = 0;
  PendingItem::Kind kind = PendingItem::Kind::Meta;

  bool operator<(const ItemKey& other) const {
    if (event_id != other.event_id) return event_id < other.event_id;
    const int a = static_cast<int>(kind);
    const int b = static_cast<int>(other.kind);
    if (a != b) return a < b;
    return seq < other.seq;
  }
};

// Clears the in-flight marker on every exit path, including the early returns
// inside attempt_item(). Quota eviction consults is_uploading(), so the marked
// window must cover the whole send (design §5.3: an event being uploaded is
// never evicted out from under the reader).
class UploadingGuard {
 public:
  UploadingGuard(Spool& spool, std::string event_id)
      : spool_(spool), event_id_(std::move(event_id)) {
    spool_.set_uploading(event_id_, true);
  }
  ~UploadingGuard() { spool_.set_uploading(event_id_, false); }
  UploadingGuard(const UploadingGuard&) = delete;
  UploadingGuard& operator=(const UploadingGuard&) = delete;

 private:
  Spool& spool_;
  std::string event_id_;
};

EventMetadata metadata_from(const Manifest& manifest) {
  EventMetadata metadata;
  metadata.event_id = manifest.event_id;
  metadata.device_id = manifest.device_id;
  metadata.event_type = manifest.event_type;  // already a wire spelling
  metadata.started_at = manifest.started_at;
  metadata.time_trusted = manifest.time_trusted;
  if (!manifest.device_boot_id.empty()) {
    metadata.device_boot_id = manifest.device_boot_id;
  }
  metadata.device_started_ms = manifest.device_started_ms;
  metadata.model_version = manifest.model_version;
  metadata.config_version = manifest.config_version;
  metadata.arm_mode = manifest.arm_mode;  // already a wire spelling
  metadata.detections = manifest.detections;
  return metadata;
}

EventClosePayload close_payload_from(const Manifest& manifest) {
  EventClosePayload payload;
  payload.ended_at = manifest.ended_at;
  payload.duration_ms = manifest.duration_ms;
  payload.time_trusted = manifest.time_trusted;
  return payload;
}

}  // namespace

int64_t backoff_delay_ms(const BackoffPolicy& policy, int attempt, uint64_t rng) {
  const int64_t cap = policy.max_ms > 0 ? policy.max_ms : 0;
  int64_t delay = policy.base_ms > 0 ? policy.base_ms : 0;
  if (attempt < 1) attempt = 1;
  // min(base * 2^(attempt-1), max_ms), computed without overflowing int64.
  for (int i = 1; i < attempt && delay < cap; ++i) {
    if (delay > cap - delay) {  // doubling would overshoot the cap
      delay = cap;
      break;
    }
    delay *= 2;
  }
  if (delay > cap) delay = cap;

  if (policy.jitter > 0.0) {
    // Factor in [1 - jitter, 1 + jitter); the unit draw is a half-open [0, 1)
    // so the product is strictly below the ceiling at the top end and the
    // result is clamped into [0, max_ms] regardless.
    const double unit = static_cast<double>(mix64(rng) >> 11) *
                        (1.0 / 9007199254740992.0);
    const double factor = (1.0 - policy.jitter) + unit * (2.0 * policy.jitter);
    double scaled = static_cast<double>(delay) * factor;
    if (scaled < 0.0) scaled = 0.0;
    if (scaled > static_cast<double>(cap)) scaled = static_cast<double>(cap);
    delay = static_cast<int64_t>(scaled);
  }
  return delay;
}

struct Uploader::Impl {
  Impl(Spool& spool_in, HubClient& client_in, IClock& clock_in,
       UploaderLimits limits_in)
      : spool(spool_in),
        client(client_in),
        clock(clock_in),
        limits(std::move(limits_in)) {}

  struct ItemResult {
    bool attempted = false;
    uint64_t bytes = 0;
  };

  ItemKey key_of(const PendingItem& item) const {
    ItemKey key;
    key.event_id = item.event_id;
    key.seq = item.kind == PendingItem::Kind::Segment ? item.seq : 0;
    key.kind = item.kind;
    return key;
  }

  // Deterministic per-item jitter source, mixed from the key hashes and the
  // attempt number (design §7.1). No RNG object and no global state, so the
  // same item always backs off by the same amount.
  uint64_t item_rng(const ItemKey& key, int attempt) const {
    uint64_t h = hash_text(key.event_id);
    h ^= static_cast<uint64_t>(static_cast<int>(key.kind) + 1) *
         0x9e3779b97f4a7c15ull;
    h ^= static_cast<uint64_t>(static_cast<uint32_t>(key.seq) + 1) *
         0xc2b2ae3d27d4eb4full;
    h ^= static_cast<uint64_t>(static_cast<uint32_t>(attempt));
    return mix64(h);
  }

  void schedule_retry(const ItemKey& key, int attempt) {
    stats.retries += 1;
    // Design §7.1: bounded exponential backoff. At the cap the item is left on
    // disk but never retried again; only quota eviction ever removes a file.
    if (attempt >= limits.backoff.max_attempts) {
      stats.permanent_failures += 1;
      blocked.insert(key);
      next_attempt_ms.erase(key);
      return;
    }
    next_attempt_ms[key] = clock.monotonic_ms() +
                           backoff_delay_ms(limits.backoff, attempt,
                                            item_rng(key, attempt));
  }

  void mark_done(const ItemKey& key) { next_attempt_ms.erase(key); }

  void block_permanently(const ItemKey& key) {
    stats.permanent_failures += 1;
    blocked.insert(key);
    next_attempt_ms.erase(key);
  }

  // 404 (hub README 错误码: "事件不存在（尚未收到元数据）"): the metadata must go
  // first, so flip it back to pending and leave the media item pending too.
  // Neither is a permanent failure.
  void requeue_meta(const std::string& event_id) {
    Manifest manifest;
    if (!spool.load_manifest(event_id, manifest).ok()) return;
    manifest.meta_uploaded = false;
    spool.update_manifest(manifest);
  }

  void handle_meta(const ItemKey& key, int attempt,
                   const EventUpsertResult& result) {
    if (result.result.ok()) {
      spool.mark_meta_uploaded(key.event_id);
      stats.meta_ok += 1;
      if (!result.created) stats.duplicates += 1;  // 200 == already present
      mark_done(key);
      return;
    }
    if (result.result.code() == Code::Invalid) {  // 422: bad metadata, no retry
      block_permanently(key);
      return;
    }
    schedule_retry(key, attempt);
  }

  void finish_media(const std::string& event_id, const ItemKey& key,
                    bool snapshot) {
    if (snapshot) {
      spool.mark_snapshot_uploaded(event_id);
      stats.snapshot_ok += 1;
    } else {
      spool.mark_segment_uploaded(event_id, key.seq);
      stats.segment_ok += 1;
    }
    mark_done(key);
  }

  void handle_media(const std::string& event_id, const ItemKey& key, int attempt,
                    UploadOutcome outcome, bool snapshot) {
    switch (outcome) {
      case UploadOutcome::Ok:
        finish_media(event_id, key, snapshot);
        break;
      case UploadOutcome::Duplicate:
        // hub README: a repeated snapshot/segment is idempotent and counts as
        // success — the hub already holds the bytes (design §7.1).
        stats.duplicates += 1;
        finish_media(event_id, key, snapshot);
        break;
      case UploadOutcome::Conflict:
        // 409: same (event, seq) with a different checksum. Resending the same
        // bytes can never resolve it, so stop retrying this item only.
        stats.conflicts += 1;
        blocked.insert(key);
        next_attempt_ms.erase(key);
        break;
      case UploadOutcome::ChecksumMismatch:
        // 400: the declared checksum disagrees with the bytes; the local file is
        // suspect. Keep it (never silently delete) but do not spin on it.
        block_permanently(key);
        break;
      case UploadOutcome::EventMissing:
        requeue_meta(event_id);
        break;
      case UploadOutcome::InsufficientStorage:
        // 507: the hub is out of room. The local file stays; retry later.
        schedule_retry(key, attempt);
        break;
      case UploadOutcome::Invalid:
        block_permanently(key);
        break;
      case UploadOutcome::NetworkError:
      case UploadOutcome::ServerError:
        schedule_retry(key, attempt);
        break;
    }
  }

  void handle_close(const std::string& event_id, const ItemKey& key, int attempt,
                    const Result& result) {
    if (result.ok()) {
      spool.mark_close_uploaded(event_id);
      stats.close_ok += 1;
      mark_done(key);
      return;
    }
    if (result.code() == Code::NotFound) {  // metadata must be (re)send first
      requeue_meta(event_id);
      return;
    }
    if (result.code() == Code::Invalid) {
      block_permanently(key);
      return;
    }
    schedule_retry(key, attempt);
  }

  ItemResult attempt_item(const PendingItem& item) {
    ItemResult out;
    const ItemKey key = key_of(item);
    const bool control = item.kind == PendingItem::Kind::Meta ||
                         item.kind == PendingItem::Kind::Close;

    Manifest manifest;
    if (control) {
      // A failed load skips the item for this pass rather than aborting the
      // whole queue (conventions: runtime paths never throw).
      if (!spool.load_manifest(item.event_id, manifest).ok()) return out;
    } else {
      std::error_code ec;
      if (!fs::exists(item.path, ec) || ec) return out;  // file gone: skip
    }

    out.attempted = true;
    out.bytes = control ? kControlBytes : item.bytes;
    const int attempt = ++attempts[key];

    UploadingGuard guard(spool, item.event_id);
    switch (item.kind) {
      case PendingItem::Kind::Meta:
        handle_meta(key, attempt, client.create_event(metadata_from(manifest)));
        break;
      case PendingItem::Kind::Snapshot:
        handle_media(item.event_id, key, attempt,
                     client.upload_snapshot(item.event_id, item.path,
                                            item.content_type),
                     true);
        break;
      case PendingItem::Kind::Segment:
        handle_media(item.event_id, key, attempt,
                     client.upload_segment(item.event_id, item.seq, item.path,
                                           item.sha256),
                     false);
        break;
      case PendingItem::Kind::Close:
        handle_close(item.event_id, key, attempt,
                     client.close_event(item.event_id,
                                        close_payload_from(manifest)));
        break;
    }
    return out;
  }

  // Design §5.3: confirmed events are retained in the uploaded_bytes bucket
  // until quota eviction reclaims them, so eviction is the only thing that ever
  // removes a local copy. Called once a pass has nothing left to do.
  void apply_quotas() {
    const QuotaReport report = spool.enforce_quotas();
    stats.events_cleaned += report.evicted_events;
  }

  bool actionable_items_remain() const { return !spool.pending(1).empty(); }

  Spool& spool;
  HubClient& client;
  IClock& clock;
  UploaderLimits limits;

  UploadStats stats;
  bool stop_requested = false;
  MonotonicMs next_send_ms = 0;

  std::map<ItemKey, int> attempts;
  std::map<ItemKey, MonotonicMs> next_attempt_ms;
  std::set<ItemKey> blocked;
};

Uploader::Uploader(Spool& spool, HubClient& client, IClock& clock,
                   UploaderLimits limits)
    : impl_(new Impl(spool, client, clock, std::move(limits))) {}

Uploader::~Uploader() = default;

size_t Uploader::run_items(size_t max_items) {
  Impl& self = *impl_;
  size_t attempted = 0;
  const std::vector<PendingItem> items = self.spool.pending(self.limits.batch);

  for (const PendingItem& item : items) {
    if (self.stop_requested) break;
    if (attempted >= max_items) break;

    const MonotonicMs now = self.clock.monotonic_ms();
    // Rate limit: stop the pass instead of sleeping. run_for() re-enters once
    // the clock has moved past the next slot (design §7.1: 不得空转占 CPU).
    if (now < self.next_send_ms) break;

    const ItemKey key = self.key_of(item);
    if (self.blocked.find(key) != self.blocked.end()) continue;  // permanent
    const auto wait = self.next_attempt_ms.find(key);
    if (wait != self.next_attempt_ms.end() && now < wait->second) continue;

    const Impl::ItemResult result = self.attempt_item(item);
    if (!result.attempted) continue;  // missing file / unreadable manifest
    attempted += 1;
    self.stats.bytes_sent += result.bytes;
    const uint64_t rate = self.limits.rate_bytes_per_s == 0
                              ? 1
                              : self.limits.rate_bytes_per_s;
    self.next_send_ms =
        now + static_cast<MonotonicMs>((result.bytes * 1000) / rate);
  }

  // Nothing left to do this pass: the queue drained, or every remaining item is
  // blocked or rate-limited. Either way this is a safe point to let quota
  // eviction reclaim confirmed events.
  if (attempted == 0 || !self.actionable_items_remain()) {
    self.apply_quotas();
  }
  return attempted;
}

void Uploader::run_for(MonotonicMs budget_ms) {
  Impl& self = *impl_;
  const MonotonicMs start = self.clock.monotonic_ms();
  for (;;) {
    if (self.stop_requested) return;
    if (self.clock.monotonic_ms() - start >= budget_ms) return;
    const size_t attempted = run_items(self.limits.batch);
    // No progress means everything is rate-limited or backing off, and the
    // injected clock cannot advance on its own; returning here is what keeps
    // run_for() from busy-looping (design §7.1).
    if (attempted == 0) return;
    if (!self.actionable_items_remain()) return;
  }
}

void Uploader::request_stop() { impl_->stop_requested = true; }

UploadStats Uploader::stats() const { return impl_->stats; }

int Uploader::attempts_for(const std::string& event_id, int seq,
                           PendingItem::Kind kind) const {
  ItemKey key;
  key.event_id = event_id;
  key.seq = kind == PendingItem::Kind::Segment ? seq : 0;
  key.kind = kind;
  const auto it = impl_->attempts.find(key);
  return it == impl_->attempts.end() ? 0 : it->second;
}

}  // namespace hv
