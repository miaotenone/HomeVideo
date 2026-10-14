// Uploader tests: ordering, idempotency, bounded backoff and rate limiting.
//

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "harness.h"
#include "httplib.h"
#include "hv/clock.h"
#include "hv/hub_client.h"
#include "hv/recorder.h"
#include "hv/spool.h"
#include "hv/uploader.h"

using namespace hv;
namespace fs = std::filesystem;

namespace {

int g_counter = 0;

// A fresh, unique spool root per case, so cases never share state.
fs::path make_root() {
  const auto stamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path root = fs::temp_directory_path() /
                        ("hv_uploader_" + std::to_string(stamp) + "_" +
                         std::to_string(++g_counter));
  std::error_code ec;
  fs::remove_all(root, ec);
  return root;
}

// Real in-process httplib server on an ephemeral port; stopped and joined in
// the destructor so a failing HV_CHECK still shuts the listener down cleanly
// (same pattern as test_hub_client.cpp).
class TestServer {
 public:
  explicit TestServer(std::function<void(httplib::Server&)> setup) {
    setup(server_);
    port_ = server_.bind_to_any_port("127.0.0.1");
    thread_ = std::thread([this] { server_.listen_after_bind(); });
    server_.wait_until_ready();
  }

  ~TestServer() {
    server_.stop();
    if (thread_.joinable()) thread_.join();
  }

  TestServer(const TestServer&) = delete;
  TestServer& operator=(const TestServer&) = delete;

  std::string url() const {
    return "http://127.0.0.1:" + std::to_string(port_);
  }

 private:
  httplib::Server server_;
  std::thread thread_;
  int port_ = 0;
};

// Lets each case build the scheduler after its stub server is up, while still
// giving it automatic storage.
class UploaderHolder {
 public:
  UploaderHolder(Spool& spool, HubClient& client, IClock& clock,
                 UploaderLimits limits)
      : value_(spool, client, clock, std::move(limits)) {}
  Uploader* operator->() { return &value_; }
  Uploader& operator*() { return value_; }

 private:
  Uploader value_;
};

Manifest make_manifest(const std::string& id,
                       const std::string& event_type = "person_intrusion") {
  Manifest manifest;
  manifest.event_id = id;
  manifest.device_id = "v831a";
  manifest.event_type = event_type;
  manifest.arm_mode = "away";
  manifest.state = "active";
  manifest.model_version = "person-yolo-v1";
  manifest.device_boot_id = "boot1";
  return manifest;
}

std::string seg_name(int seq) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "seg_%04d.bin", seq);
  return buffer;
}

FinishedSegment make_segment(int seq, size_t size) {
  FinishedSegment segment;
  segment.info.seq = seq;
  segment.info.file = seg_name(seq);
  segment.info.keyframe = true;
  segment.info.start_ms = seq * 1000;
  segment.info.end_ms = seq * 1000 + 999;
  segment.data.assign(size, static_cast<uint8_t>(seq + 1));
  segment.info.bytes = segment.data.size();
  segment.info.sha256 = sha256_hex(segment.data);
  return segment;
}

// Builds one event with exactly the confirmed/pending pieces a case wants.
void seed_event(Spool& spool, const std::string& id, bool meta_uploaded,
                bool snapshot, const std::vector<size_t>& segment_sizes,
                bool closed) {
  spool.create_event(make_manifest(id));
  if (snapshot) {
    const std::vector<uint8_t> bytes = {1, 2, 3, 4, 5, 6, 7, 8};
    spool.write_snapshot(id, bytes, "image/jpeg");
  }
  for (size_t i = 0; i < segment_sizes.size(); ++i) {
    spool.append_segment(id, make_segment(static_cast<int>(i), segment_sizes[i]));
  }
  if (meta_uploaded) spool.mark_meta_uploaded(id);
  if (closed) spool.mark_closed(id, 1760000000.0, 5000, true);
}

UploaderLimits fast_limits() {
  UploaderLimits limits;
  limits.rate_bytes_per_s = 1000000000ull;  // no intra-pass pacing
  return limits;
}

}  // namespace

HV_TEST(backoff_delay_is_exponential_capped_and_reproducible) {
  BackoffPolicy exact;
  exact.base_ms = 100;
  exact.max_ms = 1000;
  exact.jitter = 0.0;

  // jitter == 0: exactly min(base * 2^(attempt-1), max_ms).
  HV_CHECK_EQ(backoff_delay_ms(exact, 1, 0), int64_t{100});
  HV_CHECK_EQ(backoff_delay_ms(exact, 2, 0), int64_t{200});
  HV_CHECK_EQ(backoff_delay_ms(exact, 3, 0), int64_t{400});
  HV_CHECK_EQ(backoff_delay_ms(exact, 4, 0), int64_t{800});
  HV_CHECK_EQ(backoff_delay_ms(exact, 5, 0), int64_t{1000});
  HV_CHECK_EQ(backoff_delay_ms(exact, 9, 12345), int64_t{1000});

  // Non-decreasing until the cap, then flat.
  int64_t previous = -1;
  for (int attempt = 1; attempt <= 12; ++attempt) {
    const int64_t delay = backoff_delay_ms(exact, attempt, 7);
    HV_CHECK(delay >= previous);
    previous = delay;
  }

  // The same rng reproduces the same jittered delay across runs.
  BackoffPolicy jittered = exact;
  jittered.jitter = 0.5;
  const int64_t pinned = backoff_delay_ms(jittered, 3, 987654321ull);
  HV_CHECK_EQ(backoff_delay_ms(jittered, 3, 987654321ull), pinned);

  // Jitter stays in [1-j, 1+j) of the exact value and never exceeds max_ms.
  for (uint64_t rng = 1; rng <= 64; ++rng) {
    const int64_t delay = backoff_delay_ms(jittered, 2, rng);
    HV_CHECK(delay >= int64_t{100});  // 0.5 * 200
    HV_CHECK(delay < int64_t{300});   // 1.5 * 200
    HV_CHECK(delay <= jittered.max_ms);
    const int64_t capped = backoff_delay_ms(jittered, 8, rng);
    HV_CHECK(capped >= int64_t{0});
    HV_CHECK(capped <= jittered.max_ms);
  }
}

HV_TEST(items_are_sent_in_dependency_order_within_an_event) {
  const fs::path root = make_root();
  Spool spool(root, QuotaConfig{});
  HV_CHECK(spool.prepare().ok());
  seed_event(spool, "evt", /*meta_uploaded=*/false, /*snapshot=*/true,
             {32, 32, 32}, /*closed=*/true);

  std::mutex log_mutex;
  std::vector<std::string> order;
  TestServer server([&](httplib::Server& s) {
    s.Post("/api/v1/events",
           [&](const httplib::Request&, httplib::Response& res) {
             {
               std::lock_guard<std::mutex> lock(log_mutex);
               order.push_back("meta");
             }
             res.status = 201;
             res.set_content("{\"event_id\":\"evt\",\"created\":true}",
                             "application/json");
           });
    s.Post("/api/v1/events/:event_id/snapshot",
           [&](const httplib::Request&, httplib::Response& res) {
             {
               std::lock_guard<std::mutex> lock(log_mutex);
               order.push_back("snapshot");
             }
             res.status = 201;
             res.set_content("{\"status\":\"stored\"}", "application/json");
           });
    s.Put("/api/v1/events/:event_id/segments/:seq",
          [&](const httplib::Request& req, httplib::Response& res) {
            {
              std::lock_guard<std::mutex> lock(log_mutex);
              order.push_back("segment:" + req.path_params.at("seq"));
            }
            res.status = 201;
            res.set_content("{\"status\":\"stored\"}", "application/json");
          });
    s.Post("/api/v1/events/:event_id/close",
           [&](const httplib::Request&, httplib::Response& res) {
             {
               std::lock_guard<std::mutex> lock(log_mutex);
               order.push_back("close");
             }
             res.status = 200;
             res.set_content("{\"status\":\"ok\"}", "application/json");
           });
  });

  FakeClock clock;
  HubClient client(server.url(), "dev-a", 2000);
  UploaderHolder uploader(spool, client, clock, fast_limits());

  HV_CHECK_EQ(uploader->run_items(64), size_t{6});
  {
    std::lock_guard<std::mutex> lock(log_mutex);
    const std::vector<std::string> expected = {"meta",  "snapshot", "segment:0",
                                               "segment:1", "segment:2",
                                               "close"};
    HV_CHECK_EQ(order.size(), expected.size());
    if (order.size() == expected.size()) {
      for (size_t i = 0; i < expected.size(); ++i) {
        HV_CHECK_STR_EQ(order[i], expected[i]);
      }
    }
  }

  const UploadStats stats = uploader->stats();
  HV_CHECK_EQ(stats.meta_ok, uint64_t{1});
  HV_CHECK_EQ(stats.snapshot_ok, uint64_t{1});
  HV_CHECK_EQ(stats.segment_ok, uint64_t{3});
  HV_CHECK_EQ(stats.close_ok, uint64_t{1});
  HV_CHECK(spool.pending(16).empty());

  fs::remove_all(root);
}

HV_TEST(ok_and_duplicate_both_count_as_success) {
  const fs::path root = make_root();
  Spool spool(root, QuotaConfig{});
  HV_CHECK(spool.prepare().ok());
  seed_event(spool, "aaa-ok", false, true, {16}, true);
  seed_event(spool, "bbb-dup", false, true, {16}, true);

  TestServer server([&](httplib::Server& s) {
    s.Post("/api/v1/events",
           [&](const httplib::Request& req, httplib::Response& res) {
             const bool dup = req.body.find("bbb-dup") != std::string::npos;
             res.status = dup ? 200 : 201;
             res.set_content(dup ? "{\"event_id\":\"bbb-dup\",\"created\":false}"
                                 : "{\"event_id\":\"aaa-ok\",\"created\":true}",
                             "application/json");
           });
    s.Post("/api/v1/events/:event_id/snapshot",
           [&](const httplib::Request& req, httplib::Response& res) {
             res.status =
                 req.path_params.at("event_id") == "bbb-dup" ? 200 : 201;
             res.set_content("{\"status\":\"stored\"}", "application/json");
           });
    s.Put("/api/v1/events/:event_id/segments/:seq",
          [&](const httplib::Request& req, httplib::Response& res) {
            res.status =
                req.path_params.at("event_id") == "bbb-dup" ? 200 : 201;
            res.set_content("{\"status\":\"stored\"}", "application/json");
          });
    s.Post("/api/v1/events/:event_id/close",
           [](const httplib::Request&, httplib::Response& res) {
             res.status = 200;
             res.set_content("{\"status\":\"ok\"}", "application/json");
           });
  });

  FakeClock clock;
  HubClient client(server.url(), "dev-a", 2000);
  UploaderHolder uploader(spool, client, clock, fast_limits());

  uploader->run_items(64);
  const UploadStats stats = uploader->stats();
  HV_CHECK_EQ(stats.meta_ok, uint64_t{2});
  HV_CHECK_EQ(stats.snapshot_ok, uint64_t{2});
  HV_CHECK_EQ(stats.segment_ok, uint64_t{2});
  HV_CHECK_EQ(stats.close_ok, uint64_t{2});
  // Only the three 200 responses count: bbb's metadata, snapshot and segment.
  HV_CHECK_EQ(stats.duplicates, uint64_t{3});
  HV_CHECK(spool.pending(64).empty());

  fs::remove_all(root);
}

HV_TEST(duplicate_snapshot_200_is_accepted) {
  const fs::path root = make_root();
  Spool spool(root, QuotaConfig{});
  HV_CHECK(spool.prepare().ok());
  seed_event(spool, "evt", /*meta_uploaded=*/true, /*snapshot=*/true, {},
             /*closed=*/false);

  TestServer server([&](httplib::Server& s) {
    s.Post("/api/v1/events/:event_id/snapshot",
           [](const httplib::Request&, httplib::Response& res) {
             res.status = 200;
             res.set_content("{\"status\":\"duplicate\"}", "application/json");
           });
  });

  FakeClock clock;
  HubClient client(server.url(), "dev-a", 2000);
  UploaderHolder uploader(spool, client, clock, fast_limits());

  HV_CHECK_EQ(uploader->run_items(8), size_t{1});
  const UploadStats stats = uploader->stats();
  HV_CHECK_EQ(stats.snapshot_ok, uint64_t{1});
  HV_CHECK_EQ(stats.duplicates, uint64_t{1});
  HV_CHECK(spool.pending(8).empty());

  fs::remove_all(root);
}

HV_TEST(conflict_is_not_retried_and_does_not_block_other_events) {
  const fs::path root = make_root();
  Spool spool(root, QuotaConfig{});
  HV_CHECK(spool.prepare().ok());
  seed_event(spool, "aaa", false, false, {16}, false);
  seed_event(spool, "bbb", false, false, {16}, false);

  TestServer server([&](httplib::Server& s) {
    s.Post("/api/v1/events",
           [](const httplib::Request&, httplib::Response& res) {
             res.status = 201;
             res.set_content("{\"created\":true}", "application/json");
           });
    s.Put("/api/v1/events/:event_id/segments/:seq",
          [](const httplib::Request& req, httplib::Response& res) {
            if (req.path_params.at("event_id") == "aaa") {
              res.status = 409;
              res.set_content("{\"detail\":\"segment conflict\"}",
                              "application/json");
            } else {
              res.status = 201;
              res.set_content("{\"status\":\"stored\"}", "application/json");
            }
          });
  });

  FakeClock clock;
  HubClient client(server.url(), "dev-a", 2000);
  UploaderLimits limits = fast_limits();
  limits.backoff.jitter = 0.0;
  UploaderHolder uploader(spool, client, clock, limits);

  HV_CHECK_EQ(uploader->run_items(64), size_t{4});
  const UploadStats stats = uploader->stats();
  HV_CHECK_EQ(stats.conflicts, uint64_t{1});
  HV_CHECK_EQ(stats.permanent_failures, uint64_t{0});
  HV_CHECK_EQ(uploader->attempts_for("aaa", 0, PendingItem::Kind::Segment), 1);

  // "aaa"'s segment is blocked; "bbb" fully drained in the same pass.
  const std::vector<PendingItem> pending = spool.pending(64);
  HV_CHECK_EQ(pending.size(), size_t{1});
  HV_CHECK_EQ(pending[0].kind, PendingItem::Kind::Segment);
  HV_CHECK_STR_EQ(pending[0].event_id, "aaa");

  // A second pass must not retry the conflicted item.
  uploader->run_items(64);
  HV_CHECK_EQ(uploader->attempts_for("aaa", 0, PendingItem::Kind::Segment), 1);
  fs::remove_all(root);
}

HV_TEST(checksum_mismatch_is_a_permanent_failure_without_retry) {
  const fs::path root = make_root();
  Spool spool(root, QuotaConfig{});
  HV_CHECK(spool.prepare().ok());
  seed_event(spool, "evt", /*meta_uploaded=*/true, false, {16}, false);

  TestServer server([&](httplib::Server& s) {
    s.Put("/api/v1/events/:event_id/segments/:seq",
          [](const httplib::Request&, httplib::Response& res) {
            res.status = 400;
            res.set_content("{\"detail\":\"checksum mismatch\"}",
                            "application/json");
          });
  });

  FakeClock clock;
  HubClient client(server.url(), "dev-a", 2000);
  UploaderHolder uploader(spool, client, clock, fast_limits());

  HV_CHECK_EQ(uploader->run_items(8), size_t{1});
  HV_CHECK_EQ(uploader->stats().permanent_failures, uint64_t{1});
  HV_CHECK_EQ(uploader->attempts_for("evt", 0, PendingItem::Kind::Segment), 1);
  HV_CHECK_EQ(spool.pending(8).size(), size_t{1});

  uploader->run_items(8);
  HV_CHECK_EQ(uploader->attempts_for("evt", 0, PendingItem::Kind::Segment), 1);
  std::error_code ec;
  HV_CHECK(fs::exists(root / "events" / "evt" / "seg_0000.bin", ec));
  fs::remove_all(root);
}

HV_TEST(network_error_retries_to_max_then_permanent_and_keeps_files) {
  const fs::path root = make_root();
  Spool spool(root, QuotaConfig{});
  HV_CHECK(spool.prepare().ok());
  seed_event(spool, "evt", /*meta_uploaded=*/true, false, {16}, false);

  FakeClock clock;
  // Port 1 has no listener, so the connection is refused immediately.
  HubClient client("http://127.0.0.1:1", "dev-a", 500);
  UploaderLimits limits = fast_limits();
  limits.backoff.base_ms = 100;
  limits.backoff.max_ms = 10000;
  limits.backoff.max_attempts = 3;
  limits.backoff.jitter = 0.0;
  UploaderHolder uploader(spool, client, clock, limits);

  HV_CHECK_EQ(uploader->run_items(8), size_t{1});
  HV_CHECK_EQ(uploader->attempts_for("evt", 0, PendingItem::Kind::Segment), 1);

  clock.advance_ms(100);
  HV_CHECK_EQ(uploader->run_items(8), size_t{1});
  HV_CHECK_EQ(uploader->attempts_for("evt", 0, PendingItem::Kind::Segment), 2);

  clock.advance_ms(200);
  HV_CHECK_EQ(uploader->run_items(8), size_t{1});
  HV_CHECK_EQ(uploader->attempts_for("evt", 0, PendingItem::Kind::Segment), 3);

  const UploadStats stats = uploader->stats();
  HV_CHECK_EQ(stats.permanent_failures, uint64_t{1});
  HV_CHECK_EQ(stats.retries, uint64_t{3});

  // Confirmed nothing was deleted: the item and its bytes are still queued.
  const std::vector<PendingItem> pending = spool.pending(8);
  HV_CHECK_EQ(pending.size(), size_t{1});
  std::error_code ec;
  HV_CHECK(fs::exists(root / "events" / "evt" / "seg_0000.bin", ec));

  // The retry budget is spent: no further attempts.
  uploader->run_items(8);
  HV_CHECK_EQ(uploader->attempts_for("evt", 0, PendingItem::Kind::Segment), 3);
  fs::remove_all(root);
}

HV_TEST(network_error_on_one_event_still_processes_the_other) {
  const fs::path root = make_root();
  Spool spool(root, QuotaConfig{});
  HV_CHECK(spool.prepare().ok());
  seed_event(spool, "aaa", /*meta_uploaded=*/true, false, {16}, false);
  seed_event(spool, "bbb", /*meta_uploaded=*/true, false, {16}, false);

  FakeClock clock;
  // Nothing listens on port 1, so both segments fail with a transport error.
  HubClient client("http://127.0.0.1:1", "dev-a", 500);
  UploaderLimits limits = fast_limits();
  limits.backoff.jitter = 0.0;
  limits.backoff.max_attempts = 4;
  UploaderHolder uploader(spool, client, clock, limits);

  // A single pass attempts both events; the first failure does not stop the
  // second event from being worked (design §7.1).
  HV_CHECK_EQ(uploader->run_items(16), size_t{2});
  HV_CHECK_EQ(uploader->attempts_for("aaa", 0, PendingItem::Kind::Segment), 1);
  HV_CHECK_EQ(uploader->attempts_for("bbb", 0, PendingItem::Kind::Segment), 1);
  HV_CHECK_EQ(spool.pending(16).size(), size_t{2});
  fs::remove_all(root);
}

HV_TEST(a_retryable_failure_on_one_event_does_not_block_another) {
  const fs::path root = make_root();
  Spool spool(root, QuotaConfig{});
  HV_CHECK(spool.prepare().ok());
  seed_event(spool, "aaa-good", false, true, {16}, true);
  seed_event(spool, "zzz-bad", false, false, {16}, false);

  TestServer server([&](httplib::Server& s) {
    s.Post("/api/v1/events",
           [](const httplib::Request&, httplib::Response& res) {
             res.status = 201;
             res.set_content("{\"created\":true}", "application/json");
           });
    s.Post("/api/v1/events/:event_id/snapshot",
           [](const httplib::Request&, httplib::Response& res) {
             res.status = 201;
             res.set_content("{\"status\":\"stored\"}", "application/json");
           });
    s.Put("/api/v1/events/:event_id/segments/:seq",
          [](const httplib::Request& req, httplib::Response& res) {
            // 5xx is retryable, exactly like a transport failure.
            if (req.path_params.at("event_id") == "zzz-bad") {
              res.status = 500;
              res.set_content("{\"detail\":\"boom\"}", "application/json");
            } else {
              res.status = 201;
              res.set_content("{\"status\":\"stored\"}", "application/json");
            }
          });
    s.Post("/api/v1/events/:event_id/close",
           [](const httplib::Request&, httplib::Response& res) {
             res.status = 200;
             res.set_content("{\"status\":\"ok\"}", "application/json");
           });
  });

  FakeClock clock;
  HubClient client(server.url(), "dev-a", 2000);
  UploaderLimits limits = fast_limits();
  limits.backoff.jitter = 0.0;
  UploaderHolder uploader(spool, client, clock, limits);

  uploader->run_items(64);
  // The good event is fully processed even though the bad one failed.
  HV_CHECK(uploader->stats().meta_ok >= 2);
  const std::vector<PendingItem> pending = spool.pending(64);
  HV_CHECK_EQ(pending.size(), size_t{1});
  HV_CHECK_STR_EQ(pending[0].event_id, "zzz-bad");
  HV_CHECK_EQ(uploader->attempts_for("zzz-bad", 0, PendingItem::Kind::Segment),
              1);
  HV_CHECK(uploader->stats().retries >= 1);
  fs::remove_all(root);
}

HV_TEST(event_missing_requeues_metadata_then_segment_succeeds) {
  const fs::path root = make_root();
  Spool spool(root, QuotaConfig{});
  HV_CHECK(spool.prepare().ok());
  // Locally the metadata is confirmed, but the hub has forgotten the event.
  seed_event(spool, "evt", /*meta_uploaded=*/true, false, {16}, false);

  std::atomic<bool> known{false};
  TestServer server([&](httplib::Server& s) {
    s.Post("/api/v1/events",
           [&](const httplib::Request&, httplib::Response& res) {
             known = true;
             res.status = 201;
             res.set_content("{\"created\":true}", "application/json");
           });
    s.Put("/api/v1/events/:event_id/segments/:seq",
          [&](const httplib::Request&, httplib::Response& res) {
            if (!known.load()) {
              res.status = 404;
              res.set_content("{\"detail\":\"event not found\"}",
                              "application/json");
            } else {
              res.status = 201;
              res.set_content("{\"status\":\"stored\"}", "application/json");
            }
          });
  });

  FakeClock clock;
  HubClient client(server.url(), "dev-a", 2000);
  UploaderHolder uploader(spool, client, clock, fast_limits());

  // Pass 1: the segment is rejected with 404 and the metadata is re-queued.
  HV_CHECK_EQ(uploader->run_items(8), size_t{1});
  HV_CHECK_EQ(spool.pending(8).size(), size_t{2});
  HV_CHECK_EQ(spool.pending(8)[0].kind, PendingItem::Kind::Meta);

  // Pass 2: metadata first, then the segment lands.
  HV_CHECK_EQ(uploader->run_items(8), size_t{2});
  HV_CHECK(spool.pending(8).empty());
  HV_CHECK_EQ(uploader->stats().permanent_failures, uint64_t{0});
  fs::remove_all(root);
}

HV_TEST(insufficient_storage_backs_off_and_retains_file) {
  const fs::path root = make_root();
  Spool spool(root, QuotaConfig{});
  HV_CHECK(spool.prepare().ok());
  seed_event(spool, "evt", /*meta_uploaded=*/true, false, {16}, false);

  TestServer server([&](httplib::Server& s) {
    s.Put("/api/v1/events/:event_id/segments/:seq",
          [](const httplib::Request&, httplib::Response& res) {
            res.status = 507;
            res.set_content("{\"detail\":\"insufficient storage\"}",
                            "application/json");
          });
  });

  FakeClock clock;
  HubClient client(server.url(), "dev-a", 2000);
  UploaderLimits limits = fast_limits();
  limits.backoff.base_ms = 100;
  limits.backoff.max_attempts = 5;
  limits.backoff.jitter = 0.0;
  UploaderHolder uploader(spool, client, clock, limits);

  HV_CHECK_EQ(uploader->run_items(8), size_t{1});
  HV_CHECK_EQ(uploader->attempts_for("evt", 0, PendingItem::Kind::Segment), 1);
  clock.advance_ms(100);
  HV_CHECK_EQ(uploader->run_items(8), size_t{1});
  HV_CHECK_EQ(uploader->attempts_for("evt", 0, PendingItem::Kind::Segment), 2);
  clock.advance_ms(200);
  HV_CHECK_EQ(uploader->run_items(8), size_t{1});
  HV_CHECK_EQ(uploader->attempts_for("evt", 0, PendingItem::Kind::Segment), 3);

  HV_CHECK_EQ(spool.pending(8).size(), size_t{1});
  std::error_code ec;
  HV_CHECK(fs::exists(root / "events" / "evt" / "seg_0000.bin", ec));
  fs::remove_all(root);
}

HV_TEST(rate_limit_paces_a_pass_and_the_clock_releases_the_rest) {
  const fs::path root = make_root();
  Spool spool(root, QuotaConfig{});
  HV_CHECK(spool.prepare().ok());
  seed_event(spool, "evt", /*meta_uploaded=*/true, false, {1000, 1000, 1000},
             false);

  TestServer server([&](httplib::Server& s) {
    s.Put("/api/v1/events/:event_id/segments/:seq",
          [](const httplib::Request&, httplib::Response& res) {
            res.status = 201;
            res.set_content("{\"status\":\"stored\"}", "application/json");
          });
  });

  FakeClock clock;
  HubClient client(server.url(), "dev-a", 2000);
  UploaderLimits limits;
  limits.rate_bytes_per_s = 1000;  // 1000-byte segment == 1000 ms of budget
  limits.backoff.jitter = 0.0;
  UploaderHolder uploader(spool, client, clock, limits);

  // One pass sends fewer than the three pending segments.
  HV_CHECK_EQ(uploader->run_items(16), size_t{1});
  HV_CHECK_EQ(uploader->stats().bytes_sent, uint64_t{1000});
  HV_CHECK_EQ(spool.pending(16).size(), size_t{2});

  clock.advance_ms(1000);
  HV_CHECK_EQ(uploader->run_items(16), size_t{1});
  HV_CHECK_EQ(uploader->stats().bytes_sent, uint64_t{2000});

  clock.advance_ms(1000);
  HV_CHECK_EQ(uploader->run_items(16), size_t{1});
  HV_CHECK_EQ(uploader->stats().bytes_sent, uint64_t{3000});
  HV_CHECK(spool.pending(16).empty());
  fs::remove_all(root);
}

HV_TEST(request_stop_before_a_pass_leaves_the_queue_intact) {
  const fs::path root = make_root();
  Spool spool(root, QuotaConfig{});
  HV_CHECK(spool.prepare().ok());
  seed_event(spool, "evt", false, false, {16}, false);

  FakeClock clock;
  HubClient client("http://127.0.0.1:1", "dev-a", 500);
  UploaderHolder uploader(spool, client, clock, fast_limits());

  uploader->request_stop();
  HV_CHECK_EQ(uploader->run_items(8), size_t{0});
  HV_CHECK_EQ(spool.pending(8).size(), size_t{2});
  HV_CHECK_EQ(uploader->attempts_for("evt", 0, PendingItem::Kind::Meta), 0);

  uploader->run_for(1000);
  HV_CHECK_EQ(spool.pending(8).size(), size_t{2});
  fs::remove_all(root);
}

HV_TEST(confirmed_events_are_reclaimed_by_quota_eviction) {
  const fs::path root = make_root();
  QuotaConfig tight;
  tight.rolling_bytes = 0;
  tight.pending_bytes = 0;
  tight.uploaded_bytes = 0;
  tight.metadata_bytes = 0;
  Spool spool(root, tight);
  HV_CHECK(spool.prepare().ok());

  // A closed, fully-confirmed event: nothing pending, but still on disk.
  HV_CHECK(spool.create_event(make_manifest("done")).ok());
  HV_CHECK(spool.append_segment("done", make_segment(0, 256)).ok());
  HV_CHECK(spool.mark_closed("done", 1760000000.0, 1000, true).ok());
  HV_CHECK(spool.mark_meta_uploaded("done").ok());
  HV_CHECK(spool.mark_segment_uploaded("done", 0).ok());
  HV_CHECK(spool.mark_close_uploaded("done").ok());
  HV_CHECK(spool.pending(8).empty());

  FakeClock clock;
  HubClient client("http://127.0.0.1:1", "dev-a", 500);  // never contacted
  UploaderHolder uploader(spool, client, clock, fast_limits());

  uploader->run_for(1000);
  HV_CHECK_EQ(uploader->stats().events_cleaned, uint64_t{1});
  HV_CHECK(spool.pending(8).empty());
  HV_CHECK_FALSE(fs::exists(root / "events" / "done"));
  fs::remove_all(root);
}
