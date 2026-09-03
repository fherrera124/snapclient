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
  SteadyStateDriver(int32_t bufferMs, uint32_t sampleRate)
      : sampleRate_(sampleRate),
        bufferUs_(int64_t{bufferMs} * 1000) {
    engine_.onSettingsChanged(bufferMs, sampleRate);
  }

  SyncEngine& engine() { return engine_; }

  // Bootstraps playing_ the same way production code does - evaluate()
  // never returns Play directly for an initial/post-resync lock anymore,
  // only lockWithPreloadedFrames() does.
  void start(int64_t nowUs) {
    playbackStartTimeUs_ = nowUs;
    samplesWritten_ = 0;
    engine_.lockWithPreloadedFrames(0, nowUs);
    CHECK(engine_.isPlaying());
  }

  // One evaluate() call whose `age` equals ageUs exactly, then advances the
  // virtual playback clock by one test chunk.
  SyncResult step(int64_t ageUs, size_t queueDepth) {
    const int64_t actual = actualPlayLocalUs();
    const int64_t chunkServerTimeUs = actual - ageUs - bufferUs_;
    auto result = engine_.evaluate(chunkServerTimeUs, actual, queueDepth,
                                   /*dacLatencyUs=*/0, kFramesPerTestChunk);
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
constexpr int64_t kHardResyncThresholdUs = 2000;

void test_hard_resync_fires_past_threshold() {
  SteadyStateDriver driver(/*bufferMs=*/500, /*sampleRate=*/48000);
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
  SteadyStateDriver driver(/*bufferMs=*/500, /*sampleRate=*/48000);
  driver.start(/*nowUs=*/1'000'000);

  for (int i = 0; i < 150; i++) {
    auto result = driver.step(kHardResyncThresholdUs - 1, /*queueDepth=*/1);
    CHECK(result.decision == PlayDecision::Play);
    CHECK(!result.hardResync);
  }
  CHECK(driver.engine().isPlaying());
}

// steadyStateFrameAdjustment()'s shortM/miniM/age must all agree past
// threshold, or it stays 0.
void test_steady_state_gate_requires_agreement() {
  SteadyStateDriver driver(/*bufferMs=*/500, /*sampleRate=*/48000);
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

// lockWithPreloadedFrames() must seed the virtual clock at preloadedFrames,
// not 0 - a chunk landing exactly preloadedFrames later should evaluate as
// perfectly on-time.
void test_lock_with_preloaded_frames_seeds_virtual_clock() {
  constexpr uint32_t kSampleRate = 48000;
  constexpr int32_t kBufferMs = 500;
  constexpr int64_t kBufferUs = int64_t{kBufferMs} * 1000;
  constexpr int64_t kNowUs = 1'000'000;
  constexpr size_t kPreloadedFrames = 1920;  // two 960-frame chunks

  SyncEngine engine;
  engine.onSettingsChanged(kBufferMs, kSampleRate);

  engine.lockWithPreloadedFrames(kPreloadedFrames, kNowUs);
  CHECK(engine.isPlaying());

  const int64_t preloadedUs =
      static_cast<int64_t>(kPreloadedFrames) * 1'000'000 / kSampleRate;
  const int64_t evalNowUs = kNowUs + preloadedUs;
  const int64_t chunkServerTimeUs = evalNowUs - kBufferUs;

  auto result =
      engine.evaluate(chunkServerTimeUs, evalNowUs, /*queueDepth=*/1,
                      /*dacLatencyUs=*/0, kFramesPerTestChunk);
  CHECK(result.decision == PlayDecision::Play);
  CHECK(result.ageUs == 0);
}

// evaluate() must never accept a due-or-late chunk directly while not
// playing - lockWithPreloadedFrames() is the only way playing_ becomes
// true, matching master's bare age>=0 check with no grace window.
void test_initial_sync_drops_a_due_chunk_instead_of_playing_it() {
  constexpr uint32_t kSampleRate = 48000;
  constexpr int32_t kBufferMs = 500;
  constexpr int64_t kBufferUs = int64_t{kBufferMs} * 1000;
  constexpr int64_t kNowUs = 1'000'000;

  SyncEngine engine;
  engine.onSettingsChanged(kBufferMs, kSampleRate);

  // age == 0 exactly: right on time, not early - still must not play.
  auto result = engine.evaluate(kNowUs - kBufferUs, kNowUs, /*queueDepth=*/1,
                                /*dacLatencyUs=*/0, kFramesPerTestChunk);
  CHECK(result.decision == PlayDecision::DropLate);
  CHECK(!engine.isPlaying());
}

}  // namespace
}  // namespace snapclient

int main() {
  snapclient::test_hard_resync_fires_past_threshold();
  snapclient::test_hard_resync_does_not_fire_under_threshold();
  snapclient::test_steady_state_gate_requires_agreement();
  snapclient::test_lock_with_preloaded_frames_seeds_virtual_clock();
  snapclient::test_initial_sync_drops_a_due_chunk_instead_of_playing_it();

  if (gFailures == 0) {
    std::printf("all tests passed\n");
    return 0;
  }
  std::fprintf(stderr, "%d check(s) failed\n", gFailures);
  return 1;
}
