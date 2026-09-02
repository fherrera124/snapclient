#pragma once

#include <chrono>
#include <cstdint>
#include <thread>

namespace snapclient {

// What PlaybackPipeline needs to wait out a WaitMore decision. Separate
// from AudioSink since a platform can implement one without the other
// (e.g. this host default has no real audio output).
class PrecisionWaiter {
 public:
  virtual ~PrecisionWaiter() = default;
  // Starts a one-shot alarm to fire in waitUs microseconds. Non-blocking.
  // waitUs <= 0 arms nothing - the next block() call returns immediately.
  virtual void arm(int64_t waitUs) = 0;
  // Blocks until the most recently armed alarm fires.
  virtual void block() = 0;
};

// Host/test default - not precise, just correct, for builds with no
// hardware timer available.
class SleepPrecisionWaiter : public PrecisionWaiter {
 public:
  void arm(int64_t waitUs) override {
    deadline_ = waitUs > 0 ? std::chrono::steady_clock::now() +
                                 std::chrono::microseconds(waitUs)
                           : std::chrono::steady_clock::time_point{};
  }

  void block() override {
    if (deadline_ != std::chrono::steady_clock::time_point{}) {
      std::this_thread::sleep_until(deadline_);
    }
  }

 private:
  std::chrono::steady_clock::time_point deadline_{};
};

}  // namespace snapclient
