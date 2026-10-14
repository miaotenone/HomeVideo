#pragma once

// Durable per-event storage and quota enforcement.
//
// Layout (design §7):
//   spool/events/{event_id}/manifest.json   atomic write: temp file + rename
//   spool/events/{event_id}/snapshot.jpg
//   spool/events/{event_id}/seg_0000.bin
//   spool/state/device_state.json
//   spool/lost.json
//
// Nothing is cleaned up until the hub confirms it. Quota eviction order is
// fixed by design §5.3 and is the part most likely to be got wrong, so it is
// spelled out here and covered by tests.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "hv/recorder.h"
#include "hv/status.h"
#include "hv/types.h"

namespace hv {

struct ManifestSegment {
  int seq = 0;
  std::string file;
  uint64_t bytes = 0;
  std::string sha256;
  bool keyframe = false;
  MonotonicMs start_ms = 0;
  MonotonicMs end_ms = 0;
  bool uploaded = false;
};

/** Device-side record for one event; also the future source of a hub manifest. */
struct Manifest {
  int manifest_version = 1;

  std::string event_id;
  std::string device_id;
  std::string event_type;  // wire spelling
  std::optional<double> started_at;
  bool time_trusted = false;
  std::string device_boot_id;
  std::optional<int64_t> device_started_ms;
  std::string model_version;
  int config_version = 0;
  std::string arm_mode;
  std::vector<Detection> detections;

  EncoderConfig codec;

  std::vector<ManifestSegment> segments;

  std::string state;  // active | closing | closed | interrupted
  std::optional<double> ended_at;
  std::optional<int64_t> duration_ms;

  bool meta_uploaded = false;
  bool snapshot_uploaded = false;
  bool close_uploaded = false;
  std::string snapshot_content_type;
  uint64_t snapshot_bytes = 0;

  std::string to_json_string() const;
  static Result parse(const std::string& text, Manifest& out);
  Result save_atomic(const std::filesystem::path& path) const;
  static Result load(const std::filesystem::path& path, Manifest& out);

  bool closed() const { return state == "closed"; }
  uint64_t total_bytes() const;
};

struct PendingItem {
  enum class Kind { Meta, Snapshot, Segment, Close };
  Kind kind = Kind::Meta;
  std::string event_id;
  int seq = 0;
  std::filesystem::path path;
  std::string sha256;
  uint64_t bytes = 0;
  std::string content_type;
};

struct QuotaReport {
  uint64_t evicted_bytes = 0;
  uint64_t evicted_events = 0;
  std::vector<std::string> evicted_ids;
};

struct QuotaConfig {
  uint64_t rolling_bytes = 32ull * 1024 * 1024;
  uint64_t pending_bytes = 256ull * 1024 * 1024;
  uint64_t uploaded_bytes = 512ull * 1024 * 1024;
  uint64_t metadata_bytes = 16ull * 1024 * 1024;
};

class Spool {
 public:
  Spool(std::filesystem::path root, QuotaConfig quota);

  Result prepare();  // create directories, load state

  /** Creates the event directory and writes the first manifest. */
  Result create_event(const Manifest& manifest);

  Result append_segment(const std::string& event_id, const FinishedSegment& segment);
  Result write_snapshot(const std::string& event_id, const std::vector<uint8_t>& data,
                        const std::string& content_type);
  Result mark_closed(const std::string& event_id, std::optional<double> ended_at,
                     int64_t duration_ms, bool time_trusted);

  Result mark_meta_uploaded(const std::string& event_id);
  Result mark_snapshot_uploaded(const std::string& event_id);
  Result mark_segment_uploaded(const std::string& event_id, int seq);
  Result mark_close_uploaded(const std::string& event_id);

  /** Pending uploads, ordered meta -> snapshot -> segments -> close. */
  std::vector<PendingItem> pending(size_t limit) const;

  std::vector<std::string> event_ids() const;
  Result load_manifest(const std::string& event_id, Manifest& out) const;
  Result update_manifest(const Manifest& manifest);

  /** Deletes a fully-confirmed event directory. */
  Result remove_event(const std::string& event_id);

  /** Marks events that never closed before the last shutdown. */
  std::vector<std::string> mark_interrupted_events();

  /**
   * In-flight upload marker. Eviction must never take an event that is being
   * uploaded, or the rename under it would race the reader. Purely in-memory:
   * after a restart nothing is in flight.
   */
  void set_uploading(const std::string& event_id, bool uploading);
  bool is_uploading(const std::string& event_id) const;

  QuotaReport enforce_quotas();
  uint64_t bytes_used() const;
  void note_lost(uint64_t bytes, const std::string& event_id);

 private:
  std::filesystem::path event_dir(const std::string& event_id) const;
  QuotaReport evict_uploaded(uint64_t need_bytes);
  QuotaReport evict_pending(uint64_t need_bytes, bool prefer_fire_last);

  std::filesystem::path root_;
  QuotaConfig quota_;
  std::set<std::string> uploading_;
};

}  // namespace hv
