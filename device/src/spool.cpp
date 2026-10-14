#include "hv/spool.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>
#include <utility>

#include "json.hpp"
#include "json_util.h"

namespace hv {

namespace {

using nlohmann::json;
namespace fs = std::filesystem;

// Every public method that touches the filesystem wraps its body in try/catch
// and converts std::filesystem failures to Result::error(Code::Io, ...); that
// keeps the "runtime paths return Result and never throw" rule of status.h true
// at this module's boundary. The one documented exception is here.

bool blank(const std::string& text) {
  for (const char c : text) {
    if (!std::isspace(static_cast<unsigned char>(c))) return false;
  }
  return true;
}

// The caller promises ^[A-Za-z0-9._-]{1,128}$, but "." / ".." satisfy that
// charset while being path components, so they are rejected explicitly rather
// than ever escaping the events/ root.
bool valid_event_id(const std::string& id) {
  if (id.empty() || id.size() > 128) return false;
  for (const char c : id) {
    const bool allowed = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                         (c >= 'A' && c <= 'Z') || c == '.' || c == '_' ||
                         c == '-';
    if (!allowed) return false;
  }
  return id != "." && id != "..";
}

std::string segment_filename(int seq) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "seg_%04d.bin", seq);
  return buffer;
}

Result io_error(const std::string& what) {
  return Result::error(Code::Io, what);
}

// Writes through a temp file and renames into place so a crash can never
// publish a half-written file under its final name. The temp file is removed on
// every failure path.
Result write_bytes_atomic(const fs::path& path, const uint8_t* data, size_t size) {
  const fs::path tmp = path.string() + ".tmp";
  try {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      return io_error("cannot open " + tmp.string() + " for writing");
    }
    if (size > 0) {
      out.write(reinterpret_cast<const char*>(data),
                static_cast<std::streamsize>(size));
    }
    out.flush();
    if (!out.good()) {
      out.close();
      std::error_code ignored;
      fs::remove(tmp, ignored);
      return io_error("short write to " + tmp.string());
    }
    out.close();
    fs::rename(tmp, path);
    return Result::success();
  } catch (const std::exception& error) {
    std::error_code ignored;
    fs::remove(tmp, ignored);
    return io_error(std::string("write failed for ") + path.string() + ": " +
                    error.what());
  }
}

// Renamed away from file_size: passing an fs::path would otherwise pull in
// std::filesystem::file_size by ADL and make the call ambiguous.
uint64_t size_of_file(const fs::path& path) {
  std::error_code ec;
  const auto size = fs::file_size(path, ec);
  if (ec) return 0;
  return static_cast<uint64_t>(size);
}

uint64_t directory_size(const fs::path& dir) {
  uint64_t total = 0;
  std::error_code ec;
  fs::recursive_directory_iterator it(dir, ec);
  const fs::recursive_directory_iterator end;
  for (; !ec && it != end; it.increment(ec)) {
    std::error_code entry_ec;
    if (it->is_regular_file(entry_ec) && !entry_ec) {
      total += size_of_file(it->path());
    }
  }
  return total;
}

std::vector<std::string> list_event_dirs(const fs::path& events_dir) {
  std::vector<std::string> ids;
  std::error_code ec;
  fs::directory_iterator it(events_dir, ec);
  const fs::directory_iterator end;
  for (; !ec && it != end; it.increment(ec)) {
    std::error_code entry_ec;
    if (!it->is_directory(entry_ec) || entry_ec) continue;
    ids.push_back(it->path().filename().string());
  }
  return ids;
}

struct DatedId {
  std::string id;
  fs::file_time_type mtime{};
  bool has_mtime = false;
};

// Oldest first by directory mtime; ties (and filesystems without usable
// timestamps) fall back to the id so eviction order stays deterministic.
std::vector<DatedId> entries_oldest_first(const fs::path& events_dir) {
  std::vector<DatedId> entries;
  for (const std::string& id : list_event_dirs(events_dir)) {
    DatedId entry;
    entry.id = id;
    std::error_code ec;
    entry.mtime = fs::last_write_time(events_dir / id, ec);
    entry.has_mtime = !ec;
    entries.push_back(std::move(entry));
  }
  std::sort(entries.begin(), entries.end(),
            [](const DatedId& a, const DatedId& b) {
              if (a.has_mtime && b.has_mtime && a.mtime != b.mtime) {
                return a.mtime < b.mtime;
              }
              return a.id < b.id;
            });
  return entries;
}

bool snapshot_present(const fs::path& dir) {
  std::error_code ec;
  return fs::exists(dir / "snapshot.jpg", ec) && !ec;
}

// Fully uploaded = the hub has durably accepted everything the event produced.
// A missing snapshot counts as satisfied: it is never pending, so it can never
// be confirmed either.
bool fully_uploaded(const fs::path& dir, const Manifest& manifest) {
  if (!manifest.meta_uploaded || !manifest.close_uploaded) return false;
  if (snapshot_present(dir) && !manifest.snapshot_uploaded) return false;
  for (const auto& segment : manifest.segments) {
    if (!segment.uploaded) return false;
  }
  return true;
}

bool read_file(const fs::path& path, std::vector<uint8_t>& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) return false;
  out.assign(std::istreambuf_iterator<char>(in),
             std::istreambuf_iterator<char>());
  return true;
}

// One directory's worth of reclaim, folded into the report and recorded in
// lost.json. A failed unlink is not counted, so evicted_bytes always names
// space that is actually gone.
void evict_event(const fs::path& dir, const std::string& id, QuotaReport& report,
                 Spool& spool) {
  const uint64_t bytes = directory_size(dir);
  std::error_code ec;
  fs::remove_all(dir, ec);
  if (ec) return;
  report.evicted_bytes += bytes;
  report.evicted_events += 1;
  report.evicted_ids.push_back(id);
  spool.note_lost(bytes, id);
}

// -- JSON readers, mirroring config.cpp's tolerant style --
void read_string(const json& obj, const char* key, std::string& out) {
  const auto it = obj.find(key);
  if (it == obj.end() || !it->is_string()) return;
  out = it->get<std::string>();
}

void read_bool(const json& obj, const char* key, bool& out) {
  const auto it = obj.find(key);
  if (it == obj.end() || !it->is_boolean()) return;
  out = it->get<bool>();
}

void read_int(const json& obj, const char* key, int& out) {
  const auto it = obj.find(key);
  if (it == obj.end() || !it->is_number_integer()) return;
  out = static_cast<int>(it->get<long long>());
}

void read_u32(const json& obj, const char* key, uint32_t& out) {
  const auto it = obj.find(key);
  if (it == obj.end() || !it->is_number_integer()) return;
  const long long value = it->get<long long>();
  if (value >= 0) out = static_cast<uint32_t>(value);
}

void read_i64(const json& obj, const char* key, int64_t& out) {
  const auto it = obj.find(key);
  if (it == obj.end() || !it->is_number_integer()) return;
  out = it->get<long long>();
}

void read_u64(const json& obj, const char* key, uint64_t& out) {
  const auto it = obj.find(key);
  if (it == obj.end() || !it->is_number()) return;
  const double value = it->get<double>();
  if (value >= 0) out = static_cast<uint64_t>(value);
}

void read_i64_opt(const json& obj, const char* key, std::optional<int64_t>& out) {
  const auto it = obj.find(key);
  if (it == obj.end() || !it->is_number_integer()) return;
  out = it->get<long long>();
}

void read_f64_opt(const json& obj, const char* key, std::optional<double>& out) {
  const auto it = obj.find(key);
  if (it == obj.end() || !it->is_number()) return;
  out = it->get<double>();
}

}  // namespace

std::string Manifest::to_json_string() const {
  json doc;
  doc["manifest_version"] = manifest_version;
  doc["event_id"] = event_id;
  doc["device_id"] = device_id;
  doc["event_type"] = event_type;  // already a wire spelling
  doc["started_at"] = started_at.has_value() ? json(*started_at) : json(nullptr);
  doc["time_trusted"] = time_trusted;
  doc["device_boot_id"] = device_boot_id;
  doc["device_started_ms"] =
      device_started_ms.has_value() ? json(*device_started_ms) : json(nullptr);
  doc["model_version"] = model_version;
  doc["config_version"] = config_version;
  doc["arm_mode"] = arm_mode;  // wire spelling
  doc["detections"] = detections_to_json(detections);

  json codec_doc;
  codec_doc["codec"] = codec.codec;
  codec_doc["width"] = codec.width;
  codec_doc["height"] = codec.height;
  codec_doc["fps"] = codec.fps;
  codec_doc["bitrate_bps"] = codec.bitrate_bps;
  codec_doc["gop_ms"] = codec.gop_ms;
  doc["codec"] = std::move(codec_doc);

  json segments_doc = json::array();
  for (const auto& segment : segments) {
    json item;
    item["seq"] = segment.seq;
    item["file"] = segment.file;
    item["bytes"] = segment.bytes;
    item["sha256"] = segment.sha256;
    item["keyframe"] = segment.keyframe;
    item["start_ms"] = segment.start_ms;
    item["end_ms"] = segment.end_ms;
    item["uploaded"] = segment.uploaded;
    segments_doc.push_back(std::move(item));
  }
  doc["segments"] = std::move(segments_doc);

  doc["state"] = state;
  doc["ended_at"] = ended_at.has_value() ? json(*ended_at) : json(nullptr);
  doc["duration_ms"] =
      duration_ms.has_value() ? json(*duration_ms) : json(nullptr);
  doc["meta_uploaded"] = meta_uploaded;
  doc["snapshot_uploaded"] = snapshot_uploaded;
  doc["close_uploaded"] = close_uploaded;
  doc["snapshot_content_type"] = snapshot_content_type;
  doc["snapshot_bytes"] = snapshot_bytes;
  return doc.dump(2);
}

Result Manifest::parse(const std::string& text, Manifest& out) {
  json doc;
  try {
    doc = json::parse(text);
  } catch (const std::exception& error) {
    return Result::error(Code::Invalid,
                         std::string("manifest is not valid JSON: ") + error.what());
  }
  if (!doc.is_object()) {
    return Result::error(Code::Invalid, "manifest root must be a JSON object");
  }
  const auto version = doc.find("manifest_version");
  if (version == doc.end() || !version->is_number_integer() ||
      version->get<long long>() != 1) {
    return Result::error(Code::Invalid, "manifest_version must be 1");
  }
  const auto id = doc.find("event_id");
  if (id == doc.end() || !id->is_string() || blank(id->get<std::string>())) {
    return Result::error(Code::Invalid, "event_id must be a non-empty string");
  }

  Manifest manifest;
  manifest.manifest_version = 1;
  manifest.event_id = id->get<std::string>();
  read_string(doc, "device_id", manifest.device_id);
  read_string(doc, "event_type", manifest.event_type);
  read_f64_opt(doc, "started_at", manifest.started_at);
  read_bool(doc, "time_trusted", manifest.time_trusted);
  read_string(doc, "device_boot_id", manifest.device_boot_id);
  read_i64_opt(doc, "device_started_ms", manifest.device_started_ms);
  read_string(doc, "model_version", manifest.model_version);
  read_int(doc, "config_version", manifest.config_version);
  read_string(doc, "arm_mode", manifest.arm_mode);

  const auto detections = doc.find("detections");
  if (detections != doc.end() && detections->is_array()) {
    for (const auto& entry : *detections) {
      const auto detection = detection_from_json(entry);
      if (detection.has_value()) manifest.detections.push_back(*detection);
    }
  }

  const auto codec = doc.find("codec");
  if (codec != doc.end() && codec->is_object()) {
    read_string(*codec, "codec", manifest.codec.codec);
    read_u32(*codec, "width", manifest.codec.width);
    read_u32(*codec, "height", manifest.codec.height);
    read_u32(*codec, "fps", manifest.codec.fps);
    read_u32(*codec, "bitrate_bps", manifest.codec.bitrate_bps);
    read_u32(*codec, "gop_ms", manifest.codec.gop_ms);
  }

  const auto segments = doc.find("segments");
  if (segments != doc.end() && segments->is_array()) {
    for (const auto& entry : *segments) {
      if (!entry.is_object()) continue;
      ManifestSegment segment;
      read_int(entry, "seq", segment.seq);
      read_string(entry, "file", segment.file);
      read_u64(entry, "bytes", segment.bytes);
      read_string(entry, "sha256", segment.sha256);
      read_bool(entry, "keyframe", segment.keyframe);
      read_i64(entry, "start_ms", segment.start_ms);
      read_i64(entry, "end_ms", segment.end_ms);
      read_bool(entry, "uploaded", segment.uploaded);
      manifest.segments.push_back(std::move(segment));
    }
  }

  read_string(doc, "state", manifest.state);
  read_f64_opt(doc, "ended_at", manifest.ended_at);
  read_i64_opt(doc, "duration_ms", manifest.duration_ms);
  read_bool(doc, "meta_uploaded", manifest.meta_uploaded);
  read_bool(doc, "snapshot_uploaded", manifest.snapshot_uploaded);
  read_bool(doc, "close_uploaded", manifest.close_uploaded);
  read_string(doc, "snapshot_content_type", manifest.snapshot_content_type);
  read_u64(doc, "snapshot_bytes", manifest.snapshot_bytes);

  out = std::move(manifest);
  return Result::success();
}

Result Manifest::save_atomic(const fs::path& path) const {
  const std::string body = to_json_string();
  const fs::path tmp = path.string() + ".tmp";
  try {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      return io_error("cannot open " + tmp.string() + " for writing");
    }
    out << body;
    out.flush();
    if (!out.good()) {
      out.close();
      std::error_code ignored;
      fs::remove(tmp, ignored);
      return io_error("short write to " + tmp.string());
    }
    out.close();
    fs::rename(tmp, path);
    return Result::success();
  } catch (const std::exception& error) {
    std::error_code ignored;
    fs::remove(tmp, ignored);
    return io_error(std::string("manifest save failed for ") + path.string() +
                    ": " + error.what());
  }
}

Result Manifest::load(const fs::path& path, Manifest& out) {
  try {
    std::error_code ec;
    if (!fs::exists(path, ec) || ec) {
      return Result::error(Code::NotFound, "manifest not found: " + path.string());
    }
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
      return io_error("cannot read manifest: " + path.string());
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return parse(buffer.str(), out);
  } catch (const std::exception& error) {
    return io_error(std::string("manifest read failed for ") + path.string() +
                    ": " + error.what());
  }
}

uint64_t Manifest::total_bytes() const {
  uint64_t total = snapshot_bytes;
  for (const auto& segment : segments) total += segment.bytes;
  return total;
}

Spool::Spool(fs::path root, QuotaConfig quota)
    : root_(std::move(root)), quota_(quota) {}

fs::path Spool::event_dir(const std::string& event_id) const {
  return root_ / "events" / event_id;
}

Result Spool::prepare() {
  try {
    fs::create_directories(root_ / "events");
    fs::create_directories(root_ / "state");
    const fs::path lost = root_ / "lost.json";
    std::error_code ec;
    if (!fs::exists(lost, ec) || ec) {
      const std::string empty = "[]\n";
      const Result written = write_bytes_atomic(
          lost, reinterpret_cast<const uint8_t*>(empty.data()), empty.size());
      if (!written.ok()) return written;
    }
    return Result::success();
  } catch (const std::exception& error) {
    return io_error(std::string("spool prepare failed: ") + error.what());
  }
}

Result Spool::create_event(const Manifest& manifest) {
  if (!valid_event_id(manifest.event_id)) {
    return Result::error(Code::Invalid,
                         "invalid event_id: '" + manifest.event_id + "'");
  }
  try {
    const fs::path dir = event_dir(manifest.event_id);
    fs::create_directories(dir);
    const fs::path path = dir / "manifest.json";
    std::error_code ec;
    if (fs::exists(path, ec) && !ec) {
      // Idempotent: a repeated create must not drop segments already recorded.
      return Result::success();
    }
    return manifest.save_atomic(path);
  } catch (const std::exception& error) {
    return io_error(std::string("create_event failed for ") + manifest.event_id +
                    ": " + error.what());
  }
}

Result Spool::append_segment(const std::string& event_id,
                             const FinishedSegment& segment) {
  if (!valid_event_id(event_id)) {
    return Result::error(Code::Invalid, "invalid event_id: '" + event_id + "'");
  }
  Manifest manifest;
  const Result loaded = load_manifest(event_id, manifest);
  if (!loaded.ok()) return loaded;

  try {
    const fs::path dir = event_dir(event_id);
    const std::string file = segment.info.file.empty()
                                 ? segment_filename(segment.info.seq)
                                 : segment.info.file;
    const Result written =
        write_bytes_atomic(dir / file, segment.data.data(), segment.data.size());
    if (!written.ok()) return written;

    ManifestSegment record;
    record.seq = segment.info.seq;
    record.file = file;
    // Derive size from what was written so the manifest cannot disagree with
    // the bytes on disk.
    record.bytes = segment.data.size();
    record.sha256 = segment.info.sha256.empty() ? sha256_hex(segment.data)
                                                : segment.info.sha256;
    record.keyframe = segment.info.keyframe;
    record.start_ms = segment.info.start_ms;
    record.end_ms = segment.info.end_ms;
    record.uploaded = segment.info.uploaded;

    bool replaced = false;
    for (auto& existing : manifest.segments) {
      if (existing.seq == record.seq) {
        existing = record;
        replaced = true;
        break;
      }
    }
    if (!replaced) manifest.segments.push_back(record);
    std::sort(manifest.segments.begin(), manifest.segments.end(),
              [](const ManifestSegment& a, const ManifestSegment& b) {
                return a.seq < b.seq;
              });
    return manifest.save_atomic(dir / "manifest.json");
  } catch (const std::exception& error) {
    return io_error(std::string("append_segment failed for ") + event_id + ": " +
                    error.what());
  }
}

Result Spool::write_snapshot(const std::string& event_id,
                             const std::vector<uint8_t>& data,
                             const std::string& content_type) {
  if (!valid_event_id(event_id)) {
    return Result::error(Code::Invalid, "invalid event_id: '" + event_id + "'");
  }
  Manifest manifest;
  const Result loaded = load_manifest(event_id, manifest);
  if (!loaded.ok()) return loaded;

  try {
    const fs::path dir = event_dir(event_id);
    const Result written =
        write_bytes_atomic(dir / "snapshot.jpg", data.data(), data.size());
    if (!written.ok()) return written;
    manifest.snapshot_content_type = content_type;
    manifest.snapshot_bytes = data.size();
    manifest.snapshot_uploaded = false;  // a fresh snapshot must be re-confirmed
    return manifest.save_atomic(dir / "manifest.json");
  } catch (const std::exception& error) {
    return io_error(std::string("write_snapshot failed for ") + event_id + ": " +
                    error.what());
  }
}

Result Spool::mark_closed(const std::string& event_id,
                          std::optional<double> ended_at, int64_t duration_ms,
                          bool time_trusted) {
  if (!valid_event_id(event_id)) {
    return Result::error(Code::Invalid, "invalid event_id: '" + event_id + "'");
  }
  Manifest manifest;
  const Result loaded = load_manifest(event_id, manifest);
  if (!loaded.ok()) return loaded;
  manifest.state = "closed";
  manifest.ended_at = ended_at;
  manifest.duration_ms = duration_ms;
  manifest.time_trusted = time_trusted;
  try {
    return manifest.save_atomic(event_dir(event_id) / "manifest.json");
  } catch (const std::exception& error) {
    return io_error(std::string("mark_closed failed for ") + event_id + ": " +
                    error.what());
  }
}

Result Spool::mark_meta_uploaded(const std::string& event_id) {
  if (!valid_event_id(event_id)) {
    return Result::error(Code::Invalid, "invalid event_id: '" + event_id + "'");
  }
  Manifest manifest;
  const Result loaded = load_manifest(event_id, manifest);
  if (!loaded.ok()) return loaded;
  manifest.meta_uploaded = true;
  try {
    return manifest.save_atomic(event_dir(event_id) / "manifest.json");
  } catch (const std::exception& error) {
    return io_error(std::string("mark_meta_uploaded failed: ") + error.what());
  }
}

Result Spool::mark_snapshot_uploaded(const std::string& event_id) {
  if (!valid_event_id(event_id)) {
    return Result::error(Code::Invalid, "invalid event_id: '" + event_id + "'");
  }
  Manifest manifest;
  const Result loaded = load_manifest(event_id, manifest);
  if (!loaded.ok()) return loaded;
  manifest.snapshot_uploaded = true;
  try {
    return manifest.save_atomic(event_dir(event_id) / "manifest.json");
  } catch (const std::exception& error) {
    return io_error(std::string("mark_snapshot_uploaded failed: ") + error.what());
  }
}

Result Spool::mark_segment_uploaded(const std::string& event_id, int seq) {
  if (!valid_event_id(event_id)) {
    return Result::error(Code::Invalid, "invalid event_id: '" + event_id + "'");
  }
  Manifest manifest;
  const Result loaded = load_manifest(event_id, manifest);
  if (!loaded.ok()) return loaded;

  bool found = false;
  for (auto& segment : manifest.segments) {
    if (segment.seq == seq) {
      segment.uploaded = true;
      found = true;
      break;
    }
  }
  if (!found) {
    return Result::error(Code::NotFound, "segment " + std::to_string(seq) +
                                             " not found in event " + event_id);
  }
  try {
    return manifest.save_atomic(event_dir(event_id) / "manifest.json");
  } catch (const std::exception& error) {
    return io_error(std::string("mark_segment_uploaded failed: ") + error.what());
  }
}

Result Spool::mark_close_uploaded(const std::string& event_id) {
  if (!valid_event_id(event_id)) {
    return Result::error(Code::Invalid, "invalid event_id: '" + event_id + "'");
  }
  Manifest manifest;
  const Result loaded = load_manifest(event_id, manifest);
  if (!loaded.ok()) return loaded;
  manifest.close_uploaded = true;
  try {
    return manifest.save_atomic(event_dir(event_id) / "manifest.json");
  } catch (const std::exception& error) {
    return io_error(std::string("mark_close_uploaded failed: ") + error.what());
  }
}

std::vector<PendingItem> Spool::pending(size_t limit) const {
  std::vector<PendingItem> items;
  if (limit == 0) return items;
  try {
    for (const std::string& id : event_ids()) {
      Manifest manifest;
      if (!load_manifest(id, manifest).ok()) continue;
      const fs::path dir = event_dir(id);

      if (!manifest.meta_uploaded) {
        PendingItem item;
        item.kind = PendingItem::Kind::Meta;
        item.event_id = id;
        item.path = dir / "manifest.json";
        items.push_back(std::move(item));
        if (items.size() >= limit) return items;
      }

      if (!manifest.snapshot_uploaded && snapshot_present(dir)) {
        std::vector<uint8_t> data;
        if (read_file(dir / "snapshot.jpg", data)) {
          PendingItem item;
          item.kind = PendingItem::Kind::Snapshot;
          item.event_id = id;
          item.path = dir / "snapshot.jpg";
          item.sha256 = sha256_hex(data);
          item.bytes = data.size();
          item.content_type = manifest.snapshot_content_type;
          items.push_back(std::move(item));
          if (items.size() >= limit) return items;
        }
      }

      std::vector<ManifestSegment> ordered = manifest.segments;
      std::sort(ordered.begin(), ordered.end(),
                [](const ManifestSegment& a, const ManifestSegment& b) {
                  return a.seq < b.seq;
                });
      for (const auto& segment : ordered) {
        if (segment.uploaded) continue;
        PendingItem item;
        item.kind = PendingItem::Kind::Segment;
        item.event_id = id;
        item.seq = segment.seq;
        item.path = dir / segment.file;
        item.sha256 = segment.sha256;
        item.bytes = segment.bytes;
        items.push_back(std::move(item));
        if (items.size() >= limit) return items;
      }

      if (manifest.closed() && !manifest.close_uploaded) {
        PendingItem item;
        item.kind = PendingItem::Kind::Close;
        item.event_id = id;
        item.path = dir / "manifest.json";
        items.push_back(std::move(item));
        if (items.size() >= limit) return items;
      }
    }
  } catch (const std::exception&) {
    // Return whatever was gathered; the caller retries on the next pass.
  }
  return items;
}

std::vector<std::string> Spool::event_ids() const {
  std::vector<std::string> ids = list_event_dirs(root_ / "events");
  std::sort(ids.begin(), ids.end());
  return ids;
}

Result Spool::load_manifest(const std::string& event_id, Manifest& out) const {
  if (!valid_event_id(event_id)) {
    return Result::error(Code::Invalid, "invalid event_id: '" + event_id + "'");
  }
  return Manifest::load(event_dir(event_id) / "manifest.json", out);
}

Result Spool::update_manifest(const Manifest& manifest) {
  if (!valid_event_id(manifest.event_id)) {
    return Result::error(Code::Invalid,
                         "invalid event_id: '" + manifest.event_id + "'");
  }
  try {
    const fs::path dir = event_dir(manifest.event_id);
    fs::create_directories(dir);
    return manifest.save_atomic(dir / "manifest.json");
  } catch (const std::exception& error) {
    return io_error(std::string("update_manifest failed for ") +
                    manifest.event_id + ": " + error.what());
  }
}

Result Spool::remove_event(const std::string& event_id) {
  if (!valid_event_id(event_id)) {
    return Result::error(Code::Invalid, "invalid event_id: '" + event_id + "'");
  }
  try {
    const fs::path dir = event_dir(event_id);
    std::error_code ec;
    const bool existed = fs::exists(dir, ec) && !ec;
    if (!existed) {
      return Result::error(Code::NotFound, "event not found: " + event_id);
    }
    fs::remove_all(dir, ec);
    if (ec) {
      return io_error("could not remove event " + event_id + ": " + ec.message());
    }
    uploading_.erase(event_id);
    return Result::success();
  } catch (const std::exception& error) {
    return io_error(std::string("remove_event failed for ") + event_id + ": " +
                    error.what());
  }
}

std::vector<std::string> Spool::mark_interrupted_events() {
  std::vector<std::string> interrupted;
  try {
    for (const std::string& id : event_ids()) {
      Manifest manifest;
      if (!load_manifest(id, manifest).ok()) continue;
      if (manifest.state != "active" && manifest.state != "closing") continue;
      // Segments are kept: only the state changes, so the recording that did
      // land before the crash stays uploadable.
      manifest.state = "interrupted";
      if (!manifest.save_atomic(event_dir(id) / "manifest.json").ok()) continue;
      interrupted.push_back(id);
    }
  } catch (const std::exception&) {
    // Leave the rest for the next boot; ids flipped so far are returned.
  }
  return interrupted;
}

void Spool::set_uploading(const std::string& event_id, bool uploading) {
  if (uploading) {
    uploading_.insert(event_id);
  } else {
    uploading_.erase(event_id);
  }
}

bool Spool::is_uploading(const std::string& event_id) const {
  return uploading_.find(event_id) != uploading_.end();
}

uint64_t Spool::bytes_used() const {
  return directory_size(root_ / "events");
}

void Spool::note_lost(uint64_t bytes, const std::string& event_id) {
  const fs::path path = root_ / "lost.json";
  json records = json::array();
  try {
    std::ifstream in(path);
    if (in.is_open()) {
      json doc;
      in >> doc;
      if (doc.is_array()) records = std::move(doc);
    }
  } catch (const std::exception&) {
    records = json::array();  // corrupt history must not block recording new loss
  }

  bool found = false;
  for (auto& record : records) {
    if (record.is_object() &&
        record.value("event_id", std::string()) == event_id) {
      record["bytes"] = record.value("bytes", uint64_t{0}) + bytes;
      record["count"] = record.value("count", 0) + 1;
      found = true;
      break;
    }
  }
  if (!found) {
    json record;
    record["event_id"] = event_id;
    record["bytes"] = bytes;
    record["count"] = 1;
    records.push_back(std::move(record));
  }

  // Rewrite the whole file atomically: appends from another writer can never
  // leave a truncated document behind.
  const fs::path tmp = path.string() + ".tmp";
  try {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out.is_open()) return;
    out << records.dump(2);
    out.flush();
    out.close();
    fs::rename(tmp, path);
  } catch (const std::exception&) {
    std::error_code ignored;
    fs::remove(tmp, ignored);
  }
}

// Quota rule (design §5.3), implemented exactly:
//
//   need = bytes_used() - (pending_bytes + uploaded_bytes + metadata_bytes)
//   if need <= 0 -> nothing to do
//
// When over budget, events are evicted in this fixed order, stopping the moment
// the shortfall is covered:
//   1. events that are closed() AND fully uploaded (meta + snapshot + close
//      confirmed and every segment confirmed), oldest first;
//   2. events that are not fire_suspected, oldest first;
//   3. fire_suspected events, oldest first.
// The step order itself encodes "suspected fire is dropped last": a normal
// event is always evictable before a fire event, and a fire event only goes once
// nothing else is left. At every step an event whose upload is in flight
// (is_uploading()) is skipped, because deleting it would race the reader.
QuotaReport Spool::enforce_quotas() {
  const uint64_t budget =
      quota_.pending_bytes + quota_.uploaded_bytes + quota_.metadata_bytes;
  const uint64_t used = bytes_used();
  if (used <= budget) return QuotaReport{};
  const uint64_t need = used - budget;

  QuotaReport report = evict_uploaded(need);
  if (report.evicted_bytes < need) {
    const QuotaReport more = evict_pending(need - report.evicted_bytes, true);
    report.evicted_bytes += more.evicted_bytes;
    report.evicted_events += more.evicted_events;
    report.evicted_ids.insert(report.evicted_ids.end(), more.evicted_ids.begin(),
                              more.evicted_ids.end());
  }
  return report;
}

QuotaReport Spool::evict_uploaded(uint64_t need_bytes) {
  QuotaReport report;
  for (const DatedId& entry : entries_oldest_first(root_ / "events")) {
    if (report.evicted_bytes >= need_bytes) break;
    if (is_uploading(entry.id)) continue;
    Manifest manifest;
    if (!load_manifest(entry.id, manifest).ok()) continue;
    const fs::path dir = event_dir(entry.id);
    if (!manifest.closed() || !fully_uploaded(dir, manifest)) continue;
    evict_event(dir, entry.id, report, *this);
  }
  return report;
}

QuotaReport Spool::evict_pending(uint64_t need_bytes, bool prefer_fire_last) {
  QuotaReport report;
  const std::vector<DatedId> entries = entries_oldest_first(root_ / "events");

  const auto sweep = [&](bool fire_class) {
    for (const DatedId& entry : entries) {
      if (report.evicted_bytes >= need_bytes) return;
      if (is_uploading(entry.id)) continue;
      Manifest manifest;
      if (!load_manifest(entry.id, manifest).ok()) continue;
      const bool fire = manifest.event_type == "fire_suspected";
      if (fire != fire_class) continue;
      evict_event(event_dir(entry.id), entry.id, report, *this);
    }
  };

  if (prefer_fire_last) {
    sweep(false);
    if (report.evicted_bytes < need_bytes) sweep(true);
  } else {
    sweep(true);
    if (report.evicted_bytes < need_bytes) sweep(false);
  }
  return report;
}

}  // namespace hv
