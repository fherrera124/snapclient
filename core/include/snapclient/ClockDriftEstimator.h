#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>

namespace snapclient {

// Tracks how far an output running on its own clock (a DAC that is the I2S
// master) has fallen behind its nominal sample rate, in local time.
//
// Fed from the output's buffer-done interrupt: each call says how many
// frames have left since reset() and when. Interrupt latency only ever
// delays that timestamp, so each window keeps its minimum; the first
// window's minimum is the zero point, which also absorbs fixed FIFO delays.
//
// reset() and onFramesSent() belong to one context at a time (the caller
// guarantees no interrupt runs during reset()); driftUs() may be read from
// any thread.
class ClockDriftEstimator {
 public:
  static constexpr int64_t kWindowUs = 500'000;

  void reset(int64_t nowUs, uint32_t sampleRate) {
    startUs_ = nowUs;
    sampleRate_ = sampleRate;
    frames_ = 0;
    windowStartUs_ = nowUs;
    windowMin_ = std::numeric_limits<int64_t>::max();
    haveZero_ = false;
    zeroUs_ = 0;
    driftUs_.store(0, std::memory_order_relaxed);
  }

  void onFramesSent(uint32_t frames, int64_t nowUs) {
    if (sampleRate_ == 0) {
      return;
    }
    frames_ += frames;
    const int64_t lateUs =
        nowUs - startUs_ - frames_ * 1'000'000 / int64_t{sampleRate_};
    windowMin_ = std::min(windowMin_, lateUs);
    if (nowUs - windowStartUs_ < kWindowUs) {
      return;
    }
    if (!haveZero_) {
      zeroUs_ = windowMin_;
      haveZero_ = true;
    } else {
      driftUs_.store(static_cast<int32_t>(windowMin_ - zeroUs_),
                     std::memory_order_relaxed);
    }
    windowStartUs_ = nowUs;
    windowMin_ = std::numeric_limits<int64_t>::max();
  }

  // Positive: the output plays later than its nominal rate would.
  int32_t driftUs() const { return driftUs_.load(std::memory_order_relaxed); }

 private:
  int64_t startUs_ = 0;
  uint32_t sampleRate_ = 0;
  int64_t frames_ = 0;
  int64_t windowStartUs_ = 0;
  int64_t windowMin_ = std::numeric_limits<int64_t>::max();
  bool haveZero_ = false;
  int64_t zeroUs_ = 0;
  std::atomic<int32_t> driftUs_{0};
};

}  // namespace snapclient
