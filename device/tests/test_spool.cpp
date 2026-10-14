#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

#include "harness.h"
#include "hv/recorder.h"
#include "hv/spool.h"
#include "json.hpp"

using namespace hv;
namespace fs = std::filesystem;

namespace {

int g_counter = 0;

// A fresh, unique root per case keeps the cases independent without touching
// any shared state.
fs::path make_root() {
  const auto stamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path root = fs::temp_directory_path() /
                        ("hv_spool_" + std::to_string(stamp) + "_" +
                         std::to_string(++g_counter));
  std::error_code ec;
  fs::remove_all(root, ec);
  return root;
}

bool running_as_root() {
#if defined(__unix__) || defined(__APPLE__)
  return ::geteuid() == 0;
#else
  return false;
#endif
}

std::string seg_name(int seq) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "seg_%04d.bin", seq);
  return buffer;
}

Manifest make_manifest(const std::string& id,
                       const std::string& event_type = "person_intrusion",
                       const std::string& state = "active") {
  Manifest manifest;
  manifest.event_id = id;
  manifest.device_id = "v831a";
  manifest.event_type = event_type;
  manifest.arm_mode = "away";
  manifest.state = state;
  manifest.model_version = "person-yolo-v1";
  manifest.device_boot_id = "boot1";
  return manifest;
}

FinishedSegment make_segment(int seq, size_t size, bool keyframe = true) {
  FinishedSegment segment;
  segment.info.seq = seq;
  segment.info.file = seg_name(seq);
  segment.info.keyframe = keyframe;
  segment.info.start_ms = seq * 1000;
  segment.info.end_ms = seq * 1000 + 999;
  segment.data.assign(size, static_cast<uint8_t>(seq + 1));
  segment.info.bytes = segment.data.size();
  segment.info.sha256 = sha256_hex(segment.data);
  return segment;
}

std::vector<uint8_t> read_bytes(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in),
                              std::istreambuf_iterator<char>());
}

void write_text(const fs::path& path, const std::string& body) {
  std::ofstream out(path, std::ios::trunc);
  out << body;
  out.close();
}

nlohmann::json read_json(const fs::path& path) {
  std::ifstream in(path);
  nlohmann::json doc;
  in >> doc;
  return doc;
}

int tmp_files(const fs::path& dir) {
  int count = 0;
  for (const auto& entry : fs::directory_iterator(dir)) {
    const std::string name = entry.path().filename().string();
    if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".tmp") == 0) {
      count++;
    }
  }
  return count;
}

bool same_detection(const Detection& a, const Detection& b) {
  if (a.label != b.label || a.confidence != b.confidence) return false;
  if (a.bbox.has_value() != b.bbox.has_value()) return false;
  if (a.bbox.has_value() &&
      !(a.bbox->x == b.bbox->x && a.bbox->y == b.bbox->y &&
        a.bbox->w == b.bbox->w && a.bbox->h == b.bbox->h)) {
    return false;
  }
  if (a.zone != b.zone || a.ts_ms != b.ts_ms) return false;
  return true;
}

bool same_segment(const ManifestSegment& a, const ManifestSegment& b) {
  return a.seq == b.seq && a.file == b.file && a.bytes == b.bytes &&
         a.sha256 == b.sha256 && a.keyframe == b.keyframe &&
         a.start_ms == b.start_ms && a.end_ms == b.end_ms &&
         a.uploaded == b.uploaded;
}

bool manifests_equal(const Manifest& a, const Manifest& b) {
  if (a.manifest_version != b.manifest_version || a.event_id != b.event_id ||
      a.device_id != b.device_id || a.event_type != b.event_type ||
      a.started_at != b.started_at || a.time_trusted != b.time_trusted ||
      a.device_boot_id != b.device_boot_id ||
      a.device_started_ms != b.device_started_ms ||
      a.model_version != b.model_version ||
      a.config_version != b.config_version || a.arm_mode != b.arm_mode) {
    return false;
  }
  if (a.detections.size() != b.detections.size()) return false;
  for (size_t i = 0; i < a.detections.size(); ++i) {
    if (!same_detection(a.detections[i], b.detections[i])) return false;
  }
  if (a.codec.codec != b.codec.codec || a.codec.width != b.codec.width ||
      a.codec.height != b.codec.height || a.codec.fps != b.codec.fps ||
      a.codec.bitrate_bps != b.codec.bitrate_bps ||
      a.codec.gop_ms != b.codec.gop_ms) {
    return false;
  }
  if (a.segments.size() != b.segments.size()) return false;
  for (size_t i = 0; i < a.segments.size(); ++i) {
    if (!same_segment(a.segments[i], b.segments[i])) return false;
  }
  return a.state == b.state && a.ended_at == b.ended_at &&
         a.duration_ms == b.duration_ms && a.meta_uploaded == b.meta_uploaded &&
         a.snapshot_uploaded == b.snapshot_uploaded &&
         a.close_uploaded == b.close_uploaded &&
         a.snapshot_content_type == b.snapshot_content_type &&
         a.snapshot_bytes == b.snapshot_bytes;
}

}  // namespace

HV_TEST(spool_prepare_creates_layout) {
  const fs::path root = make_root();
  Spool spool(root, QuotaConfig{});
  HV_CHECK(spool.prepare().ok());
  HV_CHECK(fs::is_directory(root / "events"));
  HV_CHECK(fs::is_directory(root / "state"));
  HV_CHECK(fs::exists(root / "lost.json"));
  HV_CHECK_EQ(spool.bytes_used(), uint64_t{0});
  fs::remove_all(root);
}

HV_TEST(spool_create_event_writes_manifest_and_is_idempotent) {
  const fs::path root = make_root();
  Spool spool(root, QuotaConfig{});
  HV_CHECK(spool.prepare().ok());

  const Manifest manifest = make_manifest("evt-1");
  HV_CHECK(spool.create_event(manifest).ok());
  HV_CHECK(fs::is_directory(root / "events" / "evt-1"));
  HV_CHECK(fs::exists(root / "events" / "evt-1" / "manifest.json"));

  Manifest loaded;
  HV_CHECK(spool.load_manifest("evt-1", loaded).ok());
  HV_CHECK_STR_EQ(loaded.event_id, "evt-1");
  HV_CHECK_STR_EQ(loaded.state, "active");

  // A second create must not clobber what has already been recorded.
  HV_CHECK(spool.append_segment("evt-1", make_segment(0, 64)).ok());
  HV_CHECK(spool.create_event(manifest).ok());
  HV_CHECK(fs::exists(root / "events" / "evt-1" / "seg_0000.bin"));
  Manifest again;
  HV_CHECK(spool.load_manifest("evt-1", again).ok());
  HV_CHECK_EQ(again.segments.size(), size_t{1});

  fs::remove_all(root);
}

HV_TEST(spool_rejects_path_ish_event_ids) {
  const fs::path root = make_root();
  Spool spool(root, QuotaConfig{});
  HV_CHECK(spool.prepare().ok());

  const std::string bad_ids[] = {"", ".", "..", "a/b", "a\\b", "a b"};
  for (const std::string& id : bad_ids) {
    Manifest manifest = make_manifest(id);
    const Result result = spool.create_event(manifest);
    HV_CHECK_FALSE(result.ok());
    HV_CHECK_EQ(result.code(), Code::Invalid);
  }
  fs::remove_all(root);
}

HV_TEST(spool_append_segment_records_bytes_and_hash) {
  const fs::path root = make_root();
  Spool spool(root, QuotaConfig{});
  HV_CHECK(spool.prepare().ok());
  HV_CHECK(spool.create_event(make_manifest("evt")).ok());

  const FinishedSegment segment = make_segment(0, 4096);
  HV_CHECK(spool.append_segment("evt", segment).ok());

  const fs::path file = root / "events" / "evt" / "seg_0000.bin";
  HV_CHECK(fs::exists(file));
  const std::vector<uint8_t> on_disk = read_bytes(file);
  HV_CHECK_EQ(on_disk.size(), size_t{4096});

  Manifest manifest;
  HV_CHECK(spool.load_manifest("evt", manifest).ok());
  HV_CHECK_EQ(manifest.segments.size(), size_t{1});
  HV_CHECK_EQ(manifest.segments[0].seq, 0);
  HV_CHECK_STR_EQ(manifest.segments[0].file, "seg_0000.bin");
  HV_CHECK_EQ(manifest.segments[0].bytes, static_cast<uint64_t>(on_disk.size()));
  HV_CHECK_STR_EQ(manifest.segments[0].sha256, sha256_hex(on_disk));
  HV_CHECK_TRUE(manifest.segments[0].keyframe);

  fs::remove_all(root);
}

HV_TEST(spool_append_segment_readonly_dir_fails_cleanly) {
  const fs::path root = make_root();
  if (running_as_root()) {
    // Root bypasses the directory mode, so the write would succeed; skip.
    fs::remove_all(root);
    return;
  }
  Spool spool(root, QuotaConfig{});
  HV_CHECK(spool.prepare().ok());
  HV_CHECK(spool.create_event(make_manifest("ro")).ok());

  const fs::path dir = root / "events" / "ro";
  std::error_code ec;
  fs::permissions(dir, fs::perms::owner_write, fs::perm_options::remove, ec);
  HV_CHECK_FALSE(ec);

  const Result result = spool.append_segment("ro", make_segment(0, 128));
  HV_CHECK_FALSE(result.ok());
  HV_CHECK_EQ(result.code(), Code::Io);
  HV_CHECK_EQ(tmp_files(dir), 0);

  fs::permissions(dir, fs::perms::owner_write, fs::perm_options::add, ec);
  fs::remove_all(root);
}

HV_TEST(manifest_round_trips_every_field) {
  const fs::path root = make_root();
  fs::create_directories(root);

  Manifest manifest;
  manifest.event_id = "rt-event";
  manifest.device_id = "v831a";
  manifest.event_type = "fire_suspected";
  manifest.started_at = std::nullopt;  // serialises as JSON null
  manifest.time_trusted = false;
  manifest.device_boot_id = "boot-9";
  manifest.device_started_ms = int64_t{1234};
  manifest.model_version = "fire-yolo-v2";
  manifest.config_version = 7;
  manifest.arm_mode = "home";

  Detection person;
  person.label = EventType::PersonIntrusion;
  person.confidence = 0.82f;
  BBox box;
  box.x = 10;
  box.y = 20;
  box.w = 30;
  box.h = 40;
  person.bbox = box;
  person.zone = std::string("door");
  manifest.detections.push_back(person);
  Detection smoke;
  smoke.label = EventType::SmokeSuspected;
  smoke.confidence = 0.51f;
  manifest.detections.push_back(smoke);

  manifest.codec.codec = "h264";
  manifest.codec.width = 1280;
  manifest.codec.height = 720;
  manifest.codec.fps = 15;
  manifest.codec.bitrate_bps = 1500000;
  manifest.codec.gop_ms = 2000;

  ManifestSegment segment;
  segment.seq = 0;
  segment.file = "seg_0000.bin";
  segment.bytes = 123456;
  segment.sha256 = "deadbeef";
  segment.keyframe = true;
  segment.start_ms = 1000;
  segment.end_ms = 61000;
  segment.uploaded = true;
  manifest.segments.push_back(segment);

  manifest.state = "closed";
  manifest.ended_at = 1760000075.5;
  manifest.duration_ms = int64_t{75000};
  manifest.meta_uploaded = true;
  manifest.snapshot_uploaded = true;
  manifest.close_uploaded = true;
  manifest.snapshot_content_type = "image/jpeg";
  manifest.snapshot_bytes = 2048;

  const fs::path path = root / "manifest.json";
  HV_CHECK(manifest.save_atomic(path).ok());

  const nlohmann::json doc = nlohmann::json::parse(manifest.to_json_string());
  HV_CHECK_TRUE(doc["started_at"].is_null());
  HV_CHECK_TRUE(doc["ended_at"].is_number());
  HV_CHECK_EQ(doc["duration_ms"].get<int64_t>(), int64_t{75000});
  HV_CHECK_EQ(doc["segments"].size(), size_t{1});

  Manifest back;
  HV_CHECK(Manifest::load(path, back).ok());
  HV_CHECK_TRUE(manifests_equal(manifest, back));
  HV_CHECK_FALSE(back.started_at.has_value());
  HV_CHECK_TRUE(back.device_started_ms.has_value());
  HV_CHECK_EQ(*back.device_started_ms, int64_t{1234});
  HV_CHECK_TRUE(back.ended_at.has_value());

  fs::remove_all(root);
}

HV_TEST(manifest_round_trips_absent_optionals) {
  const fs::path root = make_root();
  fs::create_directories(root);

  Manifest manifest;
  manifest.event_id = "rt-nulls";
  manifest.state = "active";
  const fs::path path = root / "manifest.json";
  HV_CHECK(manifest.save_atomic(path).ok());

  const nlohmann::json doc = nlohmann::json::parse(manifest.to_json_string());
  HV_CHECK_TRUE(doc["started_at"].is_null());
  HV_CHECK_TRUE(doc["device_started_ms"].is_null());
  HV_CHECK_TRUE(doc["ended_at"].is_null());
  HV_CHECK_TRUE(doc["duration_ms"].is_null());

  Manifest back;
  HV_CHECK(Manifest::load(path, back).ok());
  HV_CHECK_TRUE(manifests_equal(manifest, back));

  fs::remove_all(root);
}

HV_TEST(manifest_load_rejects_bad_input) {
  const fs::path root = make_root();
  fs::create_directories(root);
  Manifest out;

  HV_CHECK_EQ(Manifest::load(root / "missing.json", out).code(), Code::NotFound);

  const fs::path blank = root / "blank.json";
  write_text(blank, "");
  HV_CHECK_EQ(Manifest::load(blank, out).code(), Code::Invalid);

  const fs::path broken = root / "broken.json";
  write_text(broken, "{\"manifest_version\":");
  HV_CHECK_EQ(Manifest::load(broken, out).code(), Code::Invalid);

  const fs::path array = root / "array.json";
  write_text(array, "[]");
  HV_CHECK_EQ(Manifest::load(array, out).code(), Code::Invalid);

  const fs::path wrong_version = root / "version.json";
  write_text(wrong_version, R"({"manifest_version": 2, "event_id": "x"})");
  HV_CHECK_EQ(Manifest::load(wrong_version, out).code(), Code::Invalid);

  const fs::path no_id = root / "no_id.json";
  write_text(no_id, R"({"manifest_version": 1})");
  HV_CHECK_EQ(Manifest::load(no_id, out).code(), Code::Invalid);

  const fs::path blank_id = root / "blank_id.json";
  write_text(blank_id, R"({"manifest_version": 1, "event_id": "   "})");
  HV_CHECK_EQ(Manifest::load(blank_id, out).code(), Code::Invalid);

  fs::remove_all(root);
}

HV_TEST(spool_pending_order_is_meta_snapshot_segments_close) {
  const fs::path root = make_root();
  Spool spool(root, QuotaConfig{});
  HV_CHECK(spool.prepare().ok());
  HV_CHECK(spool.create_event(make_manifest("evt")).ok());

  const std::vector<uint8_t> snapshot = {1, 2, 3, 4};
  HV_CHECK(spool.write_snapshot("evt", snapshot, "image/jpeg").ok());
  HV_CHECK(spool.append_segment("evt", make_segment(0, 10)).ok());
  HV_CHECK(spool.append_segment("evt", make_segment(1, 20)).ok());

  std::vector<PendingItem> items = spool.pending(64);
  HV_CHECK_EQ(items.size(), size_t{4});
  HV_CHECK_EQ(items[0].kind, PendingItem::Kind::Meta);
  HV_CHECK_STR_EQ(items[0].path, (root / "events" / "evt" / "manifest.json").string());
  HV_CHECK_EQ(items[1].kind, PendingItem::Kind::Snapshot);
  HV_CHECK_STR_EQ(items[1].content_type, "image/jpeg");
  HV_CHECK_EQ(items[1].bytes, static_cast<uint64_t>(snapshot.size()));
  HV_CHECK_STR_EQ(items[1].sha256, sha256_hex(snapshot));
  HV_CHECK_EQ(items[2].kind, PendingItem::Kind::Segment);
  HV_CHECK_EQ(items[2].seq, 0);
  HV_CHECK_EQ(items[2].bytes, uint64_t{10});
  HV_CHECK_EQ(items[3].kind, PendingItem::Kind::Segment);
  HV_CHECK_EQ(items[3].seq, 1);

  // Close only becomes pending once the event is closed.
  HV_CHECK(spool.mark_closed("evt", 1760000000.0, 75000, true).ok());
  items = spool.pending(64);
  HV_CHECK_EQ(items.size(), size_t{5});
  HV_CHECK_EQ(items[4].kind, PendingItem::Kind::Close);

  // Confirmed items disappear; segments stay until confirmed.
  HV_CHECK(spool.mark_snapshot_uploaded("evt").ok());
  items = spool.pending(64);
  HV_CHECK_EQ(items.size(), size_t{4});
  HV_CHECK_EQ(items[0].kind, PendingItem::Kind::Meta);
  HV_CHECK_EQ(items[1].kind, PendingItem::Kind::Segment);
  HV_CHECK_EQ(items[3].kind, PendingItem::Kind::Close);

  // limit caps the batch size.
  HV_CHECK_EQ(spool.pending(1).size(), size_t{1});
  HV_CHECK_EQ(spool.pending(0).size(), size_t{0});

  fs::remove_all(root);
}

HV_TEST(spool_pending_skips_missing_snapshot_and_confirmed_items) {
  const fs::path root = make_root();
  Spool spool(root, QuotaConfig{});
  HV_CHECK(spool.prepare().ok());
  HV_CHECK(spool.create_event(make_manifest("evt")).ok());

  // No snapshot file: only meta is pending even though the flag is unset.
  std::vector<PendingItem> items = spool.pending(64);
  HV_CHECK_EQ(items.size(), size_t{1});
  HV_CHECK_EQ(items[0].kind, PendingItem::Kind::Meta);

  const std::vector<uint8_t> snapshot = {9, 8, 7};
  HV_CHECK(spool.write_snapshot("evt", snapshot, "image/jpeg").ok());
  items = spool.pending(64);
  HV_CHECK_EQ(items.size(), size_t{2});
  HV_CHECK_EQ(items[1].kind, PendingItem::Kind::Snapshot);

  // Unconfirmed items never disappear between reads.
  HV_CHECK_EQ(spool.pending(64).size(), size_t{2});

  HV_CHECK(spool.mark_meta_uploaded("evt").ok());
  HV_CHECK(spool.mark_snapshot_uploaded("evt").ok());
  HV_CHECK_EQ(spool.pending(64).size(), size_t{0});

  fs::remove_all(root);
}

HV_TEST(spool_mark_segment_uploaded_unknown_seq) {
  const fs::path root = make_root();
  Spool spool(root, QuotaConfig{});
  HV_CHECK(spool.prepare().ok());
  HV_CHECK(spool.create_event(make_manifest("evt")).ok());
  HV_CHECK(spool.append_segment("evt", make_segment(0, 8)).ok());

  const Result unknown = spool.mark_segment_uploaded("evt", 5);
  HV_CHECK_FALSE(unknown.ok());
  HV_CHECK_EQ(unknown.code(), Code::NotFound);

  HV_CHECK(spool.mark_segment_uploaded("evt", 0).ok());
  Manifest manifest;
  HV_CHECK(spool.load_manifest("evt", manifest).ok());
  HV_CHECK_TRUE(manifest.segments[0].uploaded);

  HV_CHECK_EQ(spool.mark_segment_uploaded("missing-event", 0).code(),
              Code::NotFound);
  HV_CHECK_EQ(spool.mark_meta_uploaded("missing-event").code(), Code::NotFound);

  fs::remove_all(root);
}

HV_TEST(spool_quota_evicts_uploaded_before_unsent) {
  const fs::path root = make_root();
  Spool spool(root, QuotaConfig{});
  HV_CHECK(spool.prepare().ok());

  // A closed, fully-confirmed event.
  HV_CHECK(spool.create_event(make_manifest("done")).ok());
  HV_CHECK(spool.append_segment("done", make_segment(0, 800)).ok());
  HV_CHECK(spool.mark_closed("done", 1760000000.0, 1000, true).ok());
  HV_CHECK(spool.mark_meta_uploaded("done").ok());
  HV_CHECK(spool.mark_segment_uploaded("done", 0).ok());
  HV_CHECK(spool.mark_close_uploaded("done").ok());

  // An unsent event that must be kept.
  HV_CHECK(spool.create_event(make_manifest("unsent")).ok());
  HV_CHECK(spool.append_segment("unsent", make_segment(0, 800)).ok());

  const uint64_t used = spool.bytes_used();
  HV_CHECK(used > 1);
  QuotaConfig tiny;
  tiny.rolling_bytes = 0;
  tiny.pending_bytes = used - 1;
  tiny.uploaded_bytes = 0;
  tiny.metadata_bytes = 0;
  Spool tight(root, tiny);

  const QuotaReport report = tight.enforce_quotas();
  HV_CHECK_EQ(report.evicted_events, uint64_t{1});
  HV_CHECK_EQ(report.evicted_ids.size(), size_t{1});
  HV_CHECK_STR_EQ(report.evicted_ids[0], "done");
  HV_CHECK(report.evicted_bytes > 0);
  HV_CHECK_FALSE(fs::exists(root / "events" / "done"));
  HV_CHECK_TRUE(fs::exists(root / "events" / "unsent"));

  const nlohmann::json lost = read_json(root / "lost.json");
  HV_CHECK_TRUE(lost.is_array());
  HV_CHECK_EQ(lost.size(), size_t{1});
  HV_CHECK_STR_EQ(lost[0]["event_id"].get<std::string>(), "done");
  HV_CHECK_EQ(lost[0]["bytes"].get<uint64_t>(), report.evicted_bytes);
  HV_CHECK_EQ(lost[0]["count"].get<int>(), 1);

  fs::remove_all(root);
}

HV_TEST(spool_quota_evicts_person_before_fire) {
  const fs::path root = make_root();
  Spool spool(root, QuotaConfig{});
  HV_CHECK(spool.prepare().ok());
  HV_CHECK(spool.create_event(make_manifest("aaa-person", "person_intrusion")).ok());
  HV_CHECK(spool.append_segment("aaa-person", make_segment(0, 500)).ok());
  HV_CHECK(spool.create_event(make_manifest("bbb-fire", "fire_suspected")).ok());
  HV_CHECK(spool.append_segment("bbb-fire", make_segment(0, 500)).ok());

  const uint64_t used = spool.bytes_used();
  QuotaConfig tiny;
  tiny.rolling_bytes = 0;
  tiny.pending_bytes = used - 1;
  tiny.uploaded_bytes = 0;
  tiny.metadata_bytes = 0;
  Spool tight(root, tiny);

  const QuotaReport report = tight.enforce_quotas();
  HV_CHECK_EQ(report.evicted_events, uint64_t{1});
  HV_CHECK_STR_EQ(report.evicted_ids[0], "aaa-person");
  HV_CHECK_FALSE(fs::exists(root / "events" / "aaa-person"));
  HV_CHECK_TRUE(fs::exists(root / "events" / "bbb-fire"));

  fs::remove_all(root);
}

HV_TEST(spool_quota_never_evicts_uploading_event) {
  const fs::path root = make_root();
  Spool spool(root, QuotaConfig{});
  HV_CHECK(spool.prepare().ok());
  HV_CHECK(spool.create_event(make_manifest("busy", "fire_suspected")).ok());
  HV_CHECK(spool.append_segment("busy", make_segment(0, 500)).ok());

  const uint64_t used = spool.bytes_used();
  QuotaConfig tiny;
  tiny.rolling_bytes = 0;
  tiny.pending_bytes = used - 1;
  tiny.uploaded_bytes = 0;
  tiny.metadata_bytes = 0;
  Spool tight(root, tiny);

  tight.set_uploading("busy", true);
  HV_CHECK_TRUE(tight.is_uploading("busy"));
  QuotaReport report = tight.enforce_quotas();
  HV_CHECK_EQ(report.evicted_events, uint64_t{0});
  HV_CHECK_TRUE(fs::exists(root / "events" / "busy"));

  tight.set_uploading("busy", false);
  HV_CHECK_FALSE(tight.is_uploading("busy"));
  report = tight.enforce_quotas();
  HV_CHECK_EQ(report.evicted_events, uint64_t{1});
  HV_CHECK_STR_EQ(report.evicted_ids[0], "busy");
  HV_CHECK_FALSE(fs::exists(root / "events" / "busy"));

  fs::remove_all(root);
}

HV_TEST(spool_note_lost_accumulates_per_event) {
  const fs::path root = make_root();
  Spool spool(root, QuotaConfig{});
  HV_CHECK(spool.prepare().ok());

  spool.note_lost(100, "same");
  spool.note_lost(50, "same");
  spool.note_lost(7, "other");

  const nlohmann::json lost = read_json(root / "lost.json");
  HV_CHECK_TRUE(lost.is_array());
  HV_CHECK_EQ(lost.size(), size_t{2});
  HV_CHECK_STR_EQ(lost[0]["event_id"].get<std::string>(), "same");
  HV_CHECK_EQ(lost[0]["bytes"].get<uint64_t>(), uint64_t{150});
  HV_CHECK_EQ(lost[0]["count"].get<int>(), 2);
  HV_CHECK_STR_EQ(lost[1]["event_id"].get<std::string>(), "other");
  HV_CHECK_EQ(lost[1]["count"].get<int>(), 1);

  fs::remove_all(root);
}

HV_TEST(spool_mark_interrupted_events_flips_open_states) {
  const fs::path root = make_root();
  Spool spool(root, QuotaConfig{});
  HV_CHECK(spool.prepare().ok());
  HV_CHECK(spool.create_event(make_manifest("a", "person_intrusion", "active")).ok());
  HV_CHECK(spool.append_segment("a", make_segment(0, 32)).ok());
  HV_CHECK(spool.create_event(make_manifest("b", "person_intrusion", "closing")).ok());
  HV_CHECK(spool.create_event(make_manifest("c", "person_intrusion", "closed")).ok());

  const std::vector<std::string> flipped = spool.mark_interrupted_events();
  HV_CHECK_EQ(flipped.size(), size_t{2});
  HV_CHECK_STR_EQ(flipped[0], "a");
  HV_CHECK_STR_EQ(flipped[1], "b");

  Manifest a;
  HV_CHECK(spool.load_manifest("a", a).ok());
  HV_CHECK_STR_EQ(a.state, "interrupted");
  HV_CHECK_EQ(a.segments.size(), size_t{1});  // segments are kept

  Manifest b;
  HV_CHECK(spool.load_manifest("b", b).ok());
  HV_CHECK_STR_EQ(b.state, "interrupted");

  Manifest c;
  HV_CHECK(spool.load_manifest("c", c).ok());
  HV_CHECK_STR_EQ(c.state, "closed");

  HV_CHECK_EQ(spool.mark_interrupted_events().size(), size_t{0});

  fs::remove_all(root);
}

HV_TEST(spool_remove_event_drops_bytes) {
  const fs::path root = make_root();
  Spool spool(root, QuotaConfig{});
  HV_CHECK(spool.prepare().ok());
  HV_CHECK(spool.create_event(make_manifest("gone")).ok());
  HV_CHECK(spool.append_segment("gone", make_segment(0, 4096)).ok());

  const uint64_t before = spool.bytes_used();
  HV_CHECK(before >= 4096);
  HV_CHECK(spool.remove_event("gone").ok());
  HV_CHECK_FALSE(fs::exists(root / "events" / "gone"));
  HV_CHECK(spool.bytes_used() < before);
  HV_CHECK_EQ(spool.bytes_used(), uint64_t{0});
  HV_CHECK_EQ(spool.remove_event("gone").code(), Code::NotFound);

  fs::remove_all(root);
}
