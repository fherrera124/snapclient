#pragma once

#include <cstdint>

namespace snapclient {

class RateLimiter {
 public:
  explicit RateLimiter(int64_t intervalUs) : intervalUs_(intervalUs) {}
  bool due(int64_t nowUs) {
    if (nowUs - lastUs_ < intervalUs_) {
      return false;
    }
    lastUs_ = nowUs;
    return true;
  }

 private:
  int64_t intervalUs_;
  int64_t lastUs_ = 0;
};

// Shared by everything that reports lost audio: a burst must not turn into
// a burst of blocking UART writes on the path that is already struggling.
constexpr int64_t kLossLogIntervalUs = 5'000'000;

}  // namespace snapclient
