#include <cstdint>
#include <cstdio>

#include "snapclient/ClockDriftEstimator.h"

namespace {

int gFailures = 0;

void checkImpl(bool cond, const char* expr, const char* file, int line) {
  if (!cond) {
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
    gFailures++;
  }
}

}  // namespace

#define CHECK(cond) checkImpl((cond), #cond, __FILE__, __LINE__)

namespace snapclient {
namespace {

constexpr uint32_t kSampleRate = 48000;
constexpr uint32_t kFramesPerBuffer = 240;  // 5 ms at 48k

// Feeds buffers from an output whose clock runs ppm slower than nominal,
// with each interrupt delayed by jitterUs(i). Returns the time reached.
template <typename Jitter>
int64_t run(ClockDriftEstimator& est, int64_t startUs, int seconds,
            double ppm, Jitter jitterUs) {
  const double bufferUs =
      kFramesPerBuffer * 1e6 / kSampleRate * (1.0 + ppm * 1e-6);
  const int buffers = seconds * kSampleRate / kFramesPerBuffer;
  int64_t nowUs = startUs;
  for (int i = 1; i <= buffers; i++) {
    nowUs = startUs + static_cast<int64_t>(i * bufferUs);
    est.onFramesSent(kFramesPerBuffer, nowUs + jitterUs(i));
  }
  return nowUs;
}

void test_nominal_clock_reports_no_drift() {
  ClockDriftEstimator est;
  est.reset(1'000'000, kSampleRate);
  run(est, 1'000'000, 10, 0.0, [](int) { return 0; });
  CHECK(est.driftUs() == 0);
}

// A fixed FIFO delay before the first interrupt is not drift.
void test_constant_offset_is_absorbed_by_the_zero_point() {
  ClockDriftEstimator est;
  est.reset(1'000'000, kSampleRate);
  run(est, 1'000'000, 10, 0.0, [](int) { return 3000; });
  CHECK(est.driftUs() == 0);
}

void test_slow_clock_grows_positive_drift() {
  ClockDriftEstimator est;
  est.reset(0, kSampleRate);
  run(est, 0, 100, 100.0, [](int) { return 0; });
  // 100 ppm over 100 s is 10 ms, less the first window's share.
  const int32_t drift = est.driftUs();
  CHECK(drift > 9'800 && drift <= 10'000);
}

void test_fast_clock_gives_negative_drift() {
  ClockDriftEstimator est;
  est.reset(0, kSampleRate);
  run(est, 0, 100, -50.0, [](int) { return 0; });
  const int32_t drift = est.driftUs();
  CHECK(drift < -4'900 && drift >= -5'000);
}

// Interrupt latency only ever delays; the window minimum ignores it.
void test_interrupt_latency_is_rejected() {
  ClockDriftEstimator est;
  est.reset(0, kSampleRate);
  run(est, 0, 10, 0.0, [](int i) { return i % 7 == 0 ? 0 : (i * 37) % 900; });
  CHECK(est.driftUs() == 0);
}

void test_reset_clears_drift() {
  ClockDriftEstimator est;
  est.reset(0, kSampleRate);
  const int64_t endUs = run(est, 0, 20, 100.0, [](int) { return 0; });
  CHECK(est.driftUs() > 0);
  est.reset(endUs, kSampleRate);
  CHECK(est.driftUs() == 0);
  run(est, endUs, 10, 0.0, [](int) { return 0; });
  CHECK(est.driftUs() == 0);
}

void test_zero_sample_rate_is_ignored() {
  ClockDriftEstimator est;
  est.reset(0, 0);
  est.onFramesSent(kFramesPerBuffer, 1'000'000);
  CHECK(est.driftUs() == 0);
}

}  // namespace
}  // namespace snapclient

int main() {
  snapclient::test_nominal_clock_reports_no_drift();
  snapclient::test_constant_offset_is_absorbed_by_the_zero_point();
  snapclient::test_slow_clock_grows_positive_drift();
  snapclient::test_fast_clock_gives_negative_drift();
  snapclient::test_interrupt_latency_is_rejected();
  snapclient::test_reset_clears_drift();
  snapclient::test_zero_sample_rate_is_ignored();

  if (gFailures == 0) {
    std::printf("all tests passed\n");
    return 0;
  }
  std::fprintf(stderr, "%d check(s) failed\n", gFailures);
  return 1;
}
