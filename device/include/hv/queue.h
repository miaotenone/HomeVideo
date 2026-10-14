#pragma once

// Bounded queue shared by every pipeline stage.
//
// Accounting invariant (checked by tests): the number of accepted pushes equals
// popped + dropped + current depth. `rejected` counts pushes the queue refused
// outright, so it is intentionally outside that identity.

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>

namespace hv {

enum class OverflowPolicy {
  /** Evict the oldest entry to make room. Used where freshness wins. */
  DropOldest,
  /** Refuse the incoming entry, keeping what we already have. */
  DropNewest,
  /** Refuse the incoming entry without touching the queue. */
  Reject,
};

template <class T>
class BoundedQueue {
 public:
  enum class PushOutcome { Accepted, DroppedOld, DroppedNew, Rejected };

  struct Stats {
    uint64_t pushed = 0;
    uint64_t popped = 0;
    uint64_t dropped = 0;
    uint64_t rejected = 0;
    size_t depth = 0;
    size_t capacity = 0;
  };

  BoundedQueue(size_t capacity, OverflowPolicy policy)
      : capacity_(capacity), policy_(policy) {}

  BoundedQueue(const BoundedQueue&) = delete;
  BoundedQueue& operator=(const BoundedQueue&) = delete;

  PushOutcome push(T value) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (closed_ || capacity_ == 0) {
      rejected_++;
      return PushOutcome::Rejected;
    }

    PushOutcome outcome = PushOutcome::Accepted;
    if (queue_.size() >= capacity_) {
      switch (policy_) {
        case OverflowPolicy::DropOldest:
          queue_.pop_front();
          dropped_++;
          outcome = PushOutcome::DroppedOld;
          break;
        case OverflowPolicy::DropNewest:
          dropped_++;
          return PushOutcome::DroppedNew;
        case OverflowPolicy::Reject:
          rejected_++;
          return PushOutcome::Rejected;
      }
    }

    queue_.push_back(std::move(value));
    pushed_++;
    lock.unlock();
    not_empty_.notify_one();
    return outcome;
  }

  /**
   * Pops one entry. A negative `timeout_ms` waits forever. Returns false on
   * timeout, or once a closed queue has been drained.
   */
  bool pop(T& out, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto ready = [this] { return !queue_.empty() || closed_; };
    if (timeout_ms < 0) {
      not_empty_.wait(lock, ready);
    } else {
      not_empty_.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready);
    }
    if (queue_.empty()) return false;
    out = std::move(queue_.front());
    queue_.pop_front();
    popped_++;
    return true;
  }

  /** Wakes waiters and lets them drain remaining entries before pop() fails. */
  void close() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      closed_ = true;
    }
    not_empty_.notify_all();
  }

  void clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
  }

  bool closed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
  }

  size_t depth() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
  }

  size_t capacity() const { return capacity_; }

  Stats stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Stats s;
    s.pushed = pushed_;
    s.popped = popped_;
    s.dropped = dropped_;
    s.rejected = rejected_;
    s.depth = queue_.size();
    s.capacity = capacity_;
    return s;
  }

 private:
  const size_t capacity_;
  const OverflowPolicy policy_;
  mutable std::mutex mutex_;
  std::condition_variable not_empty_;
  std::deque<T> queue_;
  bool closed_ = false;
  uint64_t pushed_ = 0;
  uint64_t popped_ = 0;
  uint64_t dropped_ = 0;
  uint64_t rejected_ = 0;
};

}  // namespace hv
