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
  // Blocks the calling thread until waitUs has elapsed, waking as close to
  // that instant as the platform allows. waitUs <= 0 returns immediately.
  virtual void waitUs(int64_t waitUs) = 0;
};

// Host/test default - not precise, just correct, for builds with no
// hardware timer available.
class SleepPrecisionWaiter : public PrecisionWaiter {
 public:
  void waitUs(int64_t waitUs) override {
    if (waitUs > 0) {
      std::this_thread::sleep_for(std::chrono::microseconds(waitUs));
    }
  }
};

}  // namespace snapclient
