#pragma once

#include <cstddef>
#include <deque>
#include <iterator>
#include <set>

namespace snapclient {

// Fixed-size sliding-window median. windowSize should be odd so median()
// lands on an actual sample rather than an implied midpoint between two.
template <typename T>
class SlidingMedian {
 public:
  explicit SlidingMedian(size_t windowSize) : windowSize_(windowSize) {}

  void insert(T value) {
    order_.push_back(value);
    sorted_.insert(value);
    if (order_.size() > windowSize_) {
      sorted_.erase(sorted_.find(order_.front()));
      order_.pop_front();
    }
  }

  bool full() const { return order_.size() >= windowSize_; }

  T median() const {
    auto it = sorted_.begin();
    std::advance(it, sorted_.size() / 2);
    return *it;
  }

 private:
  size_t windowSize_;
  std::deque<T> order_;
  std::multiset<T> sorted_;
};

}  // namespace snapclient
