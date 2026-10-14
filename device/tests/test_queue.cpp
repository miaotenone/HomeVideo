#include <atomic>
#include <thread>
#include <vector>

#include "harness.h"
#include "hv/queue.h"

using namespace hv;

HV_TEST(queue_reject_policy_refuses_the_newcomer) {
  BoundedQueue<int> queue(2, OverflowPolicy::Reject);
  HV_CHECK(queue.push(1) == BoundedQueue<int>::PushOutcome::Accepted);
  HV_CHECK(queue.push(2) == BoundedQueue<int>::PushOutcome::Accepted);
  HV_CHECK(queue.push(3) == BoundedQueue<int>::PushOutcome::Rejected);
  HV_CHECK_EQ(queue.depth(), size_t{2});
  HV_CHECK_EQ(queue.stats().dropped, uint64_t{0});
  HV_CHECK_EQ(queue.stats().rejected, uint64_t{1});

  int value = 0;
  HV_CHECK(queue.pop(value, 0));
  HV_CHECK_EQ(value, 1);
  HV_CHECK(queue.pop(value, 0));
  HV_CHECK_EQ(value, 2);
}

HV_TEST(queue_drop_oldest_keeps_the_newest) {
  BoundedQueue<int> queue(2, OverflowPolicy::DropOldest);
  queue.push(1);
  queue.push(2);
  HV_CHECK(queue.push(3) == BoundedQueue<int>::PushOutcome::DroppedOld);
  HV_CHECK_EQ(queue.stats().dropped, uint64_t{1});

  int value = 0;
  HV_CHECK(queue.pop(value, 0));
  HV_CHECK_EQ(value, 2);
  HV_CHECK(queue.pop(value, 0));
  HV_CHECK_EQ(value, 3);
  HV_CHECK_FALSE(queue.pop(value, 0));
}

HV_TEST(queue_drop_newest_keeps_the_oldest) {
  BoundedQueue<int> queue(2, OverflowPolicy::DropNewest);
  queue.push(1);
  queue.push(2);
  HV_CHECK(queue.push(3) == BoundedQueue<int>::PushOutcome::DroppedNew);
  int value = 0;
  HV_CHECK(queue.pop(value, 0));
  HV_CHECK_EQ(value, 1);
  HV_CHECK(queue.pop(value, 0));
  HV_CHECK_EQ(value, 2);
}

HV_TEST(queue_pop_times_out_without_changing_accounting) {
  BoundedQueue<int> queue(1, OverflowPolicy::Reject);
  int value = 0;
  const auto before = queue.stats();
  HV_CHECK_FALSE(queue.pop(value, 20));
  const auto after = queue.stats();
  HV_CHECK_EQ(after.popped, before.popped);
  HV_CHECK_EQ(after.depth, before.depth);
}

HV_TEST(queue_closed_returns_promptly_and_still_drains) {
  BoundedQueue<int> queue(4, OverflowPolicy::Reject);
  queue.push(7);
  queue.close();

  int value = 0;
  HV_CHECK(queue.pop(value, 0));
  HV_CHECK_EQ(value, 7);

  // Empty and closed: returns immediately rather than waiting out the timeout.
  HV_CHECK_FALSE(queue.pop(value, 5000));
  HV_CHECK(queue.closed());
}

HV_TEST(queue_accounting_holds_under_concurrent_producers) {
  constexpr int kProducers = 4;
  constexpr int kPerProducer = 500;
  BoundedQueue<int> queue(8, OverflowPolicy::DropOldest);

  std::atomic<bool> producers_done{false};
  std::vector<std::thread> producers;
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&queue, p] {
      for (int i = 0; i < kPerProducer; ++i) queue.push(p * kPerProducer + i);
    });
  }
  std::thread consumer([&queue, &producers_done] {
    int value = 0;
    while (!producers_done.load()) {
      while (queue.pop(value, 1)) {
      }
    }
  });

  for (auto& producer : producers) producer.join();
  producers_done.store(true);
  consumer.join();

  // Drain whatever is left, then the identity must hold exactly.
  int value = 0;
  while (queue.pop(value, 0)) {
  }
  const auto stats = queue.stats();
  HV_CHECK_EQ(stats.pushed, stats.popped + stats.dropped + stats.depth);
  HV_CHECK_EQ(stats.depth, size_t{0});
}
