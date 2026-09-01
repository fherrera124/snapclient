#include <cstdint>
#include <cstdio>

#include "snapclient/SyncEngine.h"

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

constexpr int64_t kFramesPerTestChunk = 960;

// Drives a SyncEngine through a sequence of chunks whose evaluate()-computed
// `age` the test controls exactly, without needing a real DecoderTask/queue.
class SteadyStateDriver {
 public:
  SteadyStateDriver(size_t queueCapacity, int32_t bufferMs, uint32_t sampleRate)
      : engine_(queueCapacity),
        sampleRate_(sampleRate),
        bufferUs_(int64_t{bufferMs} * 1000) {
    engine_.onSettingsChanged(bufferMs, sampleRate);
  }

  SyncEngine& engine() { return engine_; }

  // Perfectly-on-time initial chunk, so playing_ becomes true immediately.
  void start(int64_t nowUs) {
    playbackStartTimeUs_ = nowUs;
    samplesWritten_ = 0;
    const int64_t chunkServerTimeUs = nowUs - bufferUs_;
    auto result = engine_.evaluate(chunkServerTimeUs, nowUs, /*queueDepth=*/1,
                                   /*dacLatencyUs=*/0);
    CHECK(result.decision == PlayDecision::Play);
    CHECK(engine_.isPlaying());
  }

  // One evaluate() call whose `age` equals ageUs exactly, then advances the
  // virtual playback clock by one test chunk.
  SyncResult step(int64_t ageUs, size_t queueDepth) {
    const int64_t actual = actualPlayLocalUs();
    const int64_t chunkServerTimeUs = actual - ageUs - bufferUs_;
    auto result =
        engine_.evaluate(chunkServerTimeUs, actual, queueDepth, /*dacLatencyUs=*/0);
    engine_.onFramesWritten(kFramesPerTestChunk);
    samplesWritten_ += kFramesPerTestChunk;
    return result;
  }

 private:
  int64_t actualPlayLocalUs() const {
    return playbackStartTimeUs_ +
           (samplesWritten_ * int64_t{1000000}) / sampleRate_;
  }

  SyncEngine engine_;
  uint32_t sampleRate_;
  int64_t bufferUs_;
  int64_t playbackStartTimeUs_ = 0;
  int64_t samplesWritten_ = 0;
};

// Mirrors SyncEngine.h's private kHardResyncThresholdUs - keep in sync if
// that value changes.
constexpr int64_t kHardResyncThresholdUs = 10000;

void test_hard_resync_fires_past_threshold() {
  SteadyStateDriver driver(/*queueCapacity=*/40, /*bufferMs=*/500,
                           /*sampleRate=*/48000);
  driver.start(/*nowUs=*/1'000'000);

  // shortMedian_'s 99-sample window needs 99 inserts before it's "full"
  // and the hard-resync check ever runs.
  SyncResult result;
  for (int i = 0; i < 99; i++) {
    result = driver.step(kHardResyncThresholdUs + 1, /*queueDepth=*/1);
  }
  CHECK(result.decision == PlayDecision::DropLate);
  CHECK(result.hardResync);
  CHECK(!driver.engine().isPlaying());
}

void test_hard_resync_does_not_fire_under_threshold() {
  SteadyStateDriver driver(/*queueCapacity=*/40, /*bufferMs=*/500,
                           /*sampleRate=*/48000);
  driver.start(/*nowUs=*/1'000'000);

  for (int i = 0; i < 150; i++) {
    auto result = driver.step(kHardResyncThresholdUs - 1, /*queueDepth=*/1);
    CHECK(result.decision == PlayDecision::Play);
    CHECK(!result.hardResync);
  }
  CHECK(driver.engine().isPlaying());
}

// Regression guard: scaleFrameAdjustment() must never override an
// already-positive (slow down) frameAdjustment with a queueDepth-driven
// negative one - the two must never fight in opposite directions.
void test_queue_tier_never_fights_an_active_slowdown() {
  SteadyStateDriver driver(/*queueCapacity=*/40, /*bufferMs=*/500,
                           /*sampleRate=*/48000);
  driver.start(/*nowUs=*/1'000'000);

  // Running early (negative age) past all three steadyStateFrameAdjustment
  // thresholds fills the medians enough to request frameAdjustment = +1.
  SyncResult result;
  for (int i = 0; i < 99; i++) {
    result = driver.step(-200, /*queueDepth=*/1);
  }
  CHECK(result.decision == PlayDecision::Play);
  CHECK(result.frameAdjustment > 0);

  // A queueDepth right at the bounded queue's capacity would, if the guard
  // were broken, force frameAdjustment negative to drain the queue.
  result = driver.step(-200, /*queueDepth=*/40);
  CHECK(result.decision == PlayDecision::Play);
  CHECK(result.frameAdjustment > 0);
}

// A large queueDepth on a flat-age signal (frameAdjustment otherwise 0) is
// the one case queueDepth tiers ARE allowed to push negative.
void test_queue_tier_drains_on_flat_age() {
  SteadyStateDriver driver(/*queueCapacity=*/40, /*bufferMs=*/100,
                           /*sampleRate=*/48000);
  driver.start(/*nowUs=*/1'000'000);

  SyncResult result;
  for (int i = 0; i < 99; i++) {
    result = driver.step(0, /*queueDepth=*/1);
  }
  CHECK(result.frameAdjustment == 0);

  // targetQueue = bufferMs/20 = 5; the widest tier's threshold here is
  // min(5+11, 40-2) = 16.
  result = driver.step(0, /*queueDepth=*/20);
  CHECK(result.decision == PlayDecision::Play);
  CHECK(result.frameAdjustment < 0);
}

// steadyStateFrameAdjustment()'s shortM/miniM/age must all agree past
// threshold, or it stays 0.
void test_steady_state_gate_requires_agreement() {
  SteadyStateDriver driver(/*queueCapacity=*/40, /*bufferMs=*/500,
                           /*sampleRate=*/48000);
  driver.start(/*nowUs=*/1'000'000);

  // Fill both medians with a steady early-running age.
  for (int i = 0; i < 99; i++) {
    driver.step(-200, /*queueDepth=*/1);
  }

  // This call's own instantaneous age disagrees with the (still mostly
  // -200) medians - the gate must reject it despite shortM/miniM alone
  // still looking like a clear early-running case.
  auto result = driver.step(0, /*queueDepth=*/1);
  CHECK(result.frameAdjustment == 0);
}

}  // namespace
}  // namespace snapclient

int main() {
  snapclient::test_hard_resync_fires_past_threshold();
  snapclient::test_hard_resync_does_not_fire_under_threshold();
  snapclient::test_queue_tier_never_fights_an_active_slowdown();
  snapclient::test_queue_tier_drains_on_flat_age();
  snapclient::test_steady_state_gate_requires_agreement();

  if (gFailures == 0) {
    std::printf("all tests passed\n");
    return 0;
  }
  std::fprintf(stderr, "%d check(s) failed\n", gFailures);
  return 1;
}
