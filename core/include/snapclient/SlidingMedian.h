#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace snapclient {

// Fixed-size sliding-window median. windowSize should be odd so median()
// lands on an actual sample rather than an implied midpoint between two.
// Both buffers are sized once at construction and never reallocate after -
// insert() only ever shifts elements within that fixed capacity, so
// steady-state operation does no heap allocation.
template <typename T>
class SlidingMedian {
 public:
  explicit SlidingMedian(size_t windowSize) : windowSize_(windowSize) {
    ring_.resize(windowSize_);
    sorted_.reserve(windowSize_);
  }

  void insert(T value) {
    if (count_ == windowSize_) {
      const T evicted = ring_[head_];
      sorted_.erase(std::lower_bound(sorted_.begin(), sorted_.end(), evicted));
    } else {
      count_++;
    }
    ring_[head_] = value;
    head_ = (head_ + 1) % windowSize_;
    sorted_.insert(std::upper_bound(sorted_.begin(), sorted_.end(), value),
                   value);
  }

  bool full() const { return count_ >= windowSize_; }

  void clear() {
    sorted_.clear();
    count_ = 0;
    head_ = 0;
  }

  T median() const { return sorted_[sorted_.size() / 2]; }

 private:
  size_t windowSize_;
  std::vector<T> ring_;
  std::vector<T> sorted_;
  size_t count_ = 0;
  size_t head_ = 0;
};

}  // namespace snapclient
