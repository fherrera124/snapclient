#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>

#include <bell/utils/Semaphore.h>

namespace snapclient {

// Thread-safe bounded FIFO. One mutex plus two semaphores (items
// available, free slots) guard a std::deque capped at a capacity that's
// adjustable at runtime via setCapacity() - every public method is a
// self-contained critical section that never calls into another
// BoundedQueue, so instances never need lock-ordering discipline against
// each other.
template <typename T>
class BoundedQueue {
 public:
  explicit BoundedQueue(size_t capacity)
      : capacity_(capacity), freeSlots_(static_cast<uint32_t>(capacity)) {}

  // Never blocks. Returns false (item untouched) if already at capacity -
  // for a producer that must never stall (e.g. a network read thread).
  bool tryPush(T item) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (deque_.size() >= capacity_) {
      return false;
    }
    pushLocked(std::move(item));
    return true;
  }

  // Blocks until a slot is free - deliberate backpressure for a producer
  // that should pause when the consumer hasn't caught up.
  void push(T item) {
    while (!freeSlots_.take(10)) {
    }
    std::lock_guard<std::mutex> lock(mutex_);
    pushLocked(std::move(item));
  }

  // Pops the front item into out and returns true, or waits up to
  // timeoutMs for one to arrive and returns false on timeout.
  bool tryPop(T& out, int timeoutMs) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!deque_.empty()) {
        out = std::move(deque_.front());
        deque_.pop_front();
        freeSlots_.give();
        return true;
      }
    }
    if (!itemAvailable_.take(timeoutMs)) {
      return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (deque_.empty()) {
      return false;
    }
    out = std::move(deque_.front());
    deque_.pop_front();
    freeSlots_.give();
    return true;
  }

  size_t size() {
    std::lock_guard<std::mutex> lock(mutex_);
    return deque_.size();
  }

  size_t capacity() {
    std::lock_guard<std::mutex> lock(mutex_);
    return capacity_;
  }

  // Only meant for a tryPush()/tryPop() producer/consumer pair: freeSlots_
  // only grows here, never shrinks, so a push()-blocking producer could
  // still overfill briefly right after a decrease, until enough items
  // drain to bring the semaphore back in line.
  void setCapacity(size_t newCapacity) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (newCapacity > capacity_) {
      for (size_t i = capacity_; i < newCapacity; i++) {
        freeSlots_.give();
      }
    }
    capacity_ = newCapacity;
  }

  void clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < deque_.size(); i++) {
      freeSlots_.give();
    }
    deque_.clear();
  }

 private:
  void pushLocked(T item) {
    const bool wasEmpty = deque_.empty();
    deque_.push_back(std::move(item));
    if (wasEmpty) {
      itemAvailable_.give();
    }
  }

  size_t capacity_;
  std::mutex mutex_;
  std::deque<T> deque_;
  bell::Semaphore itemAvailable_;
  bell::Semaphore freeSlots_;
};

}  // namespace snapclient
