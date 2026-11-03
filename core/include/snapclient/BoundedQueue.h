#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>

namespace snapclient {

// Thread-safe bounded FIFO whose capacity can change at runtime. Every
// public method is a self-contained critical section that never calls into
// another BoundedQueue, so instances never need lock-ordering discipline
// against each other.
template <typename T>
class BoundedQueue {
 public:
  explicit BoundedQueue(size_t capacity) : capacity_(capacity) {}

  // Never blocks. Returns false (item untouched) if already at capacity -
  // for a producer that must never stall (e.g. a network read thread).
  bool tryPush(T item) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (deque_.size() >= capacity_) {
        return false;
      }
      deque_.push_back(std::move(item));
    }
    notEmpty_.notify_one();
    return true;
  }

  // Blocks until a slot is free - deliberate backpressure for a producer
  // that should pause when the consumer hasn't caught up.
  void push(T item) {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      notFull_.wait(lock, [this] { return deque_.size() < capacity_; });
      deque_.push_back(std::move(item));
    }
    notEmpty_.notify_one();
  }

  // Pops the front item into out and returns true, or waits up to
  // timeoutMs for one to arrive and returns false on timeout. 0 never
  // waits.
  bool tryPop(T& out, int timeoutMs) {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (!notEmpty_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                              [this] { return !deque_.empty(); })) {
        return false;
      }
      out = std::move(deque_.front());
      deque_.pop_front();
    }
    notFull_.notify_one();
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

  void setCapacity(size_t newCapacity) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      capacity_ = newCapacity;
    }
    notFull_.notify_all();
  }

  void clear() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      deque_.clear();
    }
    notFull_.notify_all();
  }

 private:
  size_t capacity_;
  std::mutex mutex_;
  std::condition_variable notEmpty_;
  std::condition_variable notFull_;
  std::deque<T> deque_;
};

}  // namespace snapclient
