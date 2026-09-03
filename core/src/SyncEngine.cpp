#include "snapclient/SyncEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include <bell/Logger.h>

namespace snapclient {

namespace {
const char* kLogTag = "SyncEngine";
}  // namespace

SyncEngine::SyncEngine()
    : timeFilter_(0.01, 0.0, 1.001, 0.75, 100, 2.0),
      shortMedian_(99),
      miniMedian_(19) {}

void SyncEngine::onSettingsChanged(int32_t bufferMs, uint32_t sampleRate) {
  bufferMs_ = bufferMs;
  sampleRate_ = sampleRate;
  playing_ = false;
  shortMedian_.clear();
  miniMedian_.clear();
}

void SyncEngine::insertLatencySample(int64_t offsetUs, int64_t maxErrorUs,
                                     int64_t nowUs) {
  timeFilter_.insert(offsetUs, maxErrorUs, nowUs);
}

bool SyncEngine::latencyReady() const {
  return timeFilter_.isFull(kLatencyFilterFull);
}

void SyncEngine::reset() {
  playing_ = false;
  shortMedian_.clear();
  miniMedian_.clear();
  timeFilter_.reset();
}

SyncResult SyncEngine::evaluate(int64_t chunkServerTimeUs, int64_t nowUs,
                                size_t queueDepth, int32_t dacLatencyUs,
                                size_t chunkFrames, int64_t minLockLeadUs) {
  const int64_t diffToServer = timeFilter_.offsetAt(nowUs);
  const int64_t serverNowUs = nowUs + diffToServer;
  const int64_t bufferUs = int64_t{bufferMs_} * 1000;

  if (!playing_) {
    const int64_t age =
        serverNowUs - chunkServerTimeUs - bufferUs + dacLatencyUs;
    if (age < -minLockLeadUs) {
      return {PlayDecision::WaitMore, -age, 0, false};
    }
    // lockWithPreloadedFrames() is the only way playing_ becomes true, so
    // there is no grace window - skip, in one pass, to a chunk with enough
    // lead.
    const int64_t chunkDurationUs =
        static_cast<int64_t>(std::max<size_t>(chunkFrames, 1)) * 1'000'000 /
        sampleRate_;
    const int chunksToSkip = static_cast<int>(
        (age + minLockLeadUs + chunkDurationUs - 1) / chunkDurationUs);
    BELL_LOG(warn, kLogTag,
             "initial sync: age={} diffToServer={} bufferUs={} "
             "dacLatencyUs={} skipping {} more",
             age, diffToServer, bufferUs, dacLatencyUs, chunksToSkip);
    SyncResult result;
    result.decision = PlayDecision::DropLate;
    result.chunksToSkip = chunksToSkip;
    return result;
  }

  if (queueDepth == 0) {
    playing_ = false;
    shortMedian_.clear();
    miniMedian_.clear();
    return {PlayDecision::DropLate, 0, 0, true};
  }

  // Both sides are write frontiers, so the DMA backlog sits on both and
  // cancels - it must not be subtracted here.
  const int64_t actualPlayLocalUs =
      playbackStartTimeUs_ + (samplesWritten_ * int64_t{1000000}) / sampleRate_;
  const int64_t targetPlayLocalUs =
      chunkServerTimeUs - diffToServer + bufferUs - dacLatencyUs;
  const int64_t age = actualPlayLocalUs - targetPlayLocalUs;

  shortMedian_.insert(age);
  miniMedian_.insert(age);

  if (shortMedian_.full() && std::abs(shortMedian_.median()) > kHardResyncThresholdUs) {
    BELL_LOG(warn, kLogTag,
             "hard resync: age={} shortMedian={} miniMedian={} "
             "diffToServer={} dacLatencyUs={}",
             age, shortMedian_.median(), miniMedian_.median(), diffToServer,
             dacLatencyUs);
    playing_ = false;
    shortMedian_.clear();
    miniMedian_.clear();
    return {PlayDecision::DropLate, 0, 0, true};
  }

  // Shorter window than the hard resync above on purpose: correction must
  // be able to start before that can trigger. median() is valid partly full.
  int frameAdjustment = 0;
  if (miniMedian_.full()) {
    frameAdjustment = steadyStateFrameAdjustment(
        shortMedian_.median(), miniMedian_.median(), age);
  }

  return {PlayDecision::Play, 0, frameAdjustment, false, age, diffToServer};
}

void SyncEngine::onFramesWritten(size_t frameCount) {
  samplesWritten_ += static_cast<int64_t>(frameCount);
}

void SyncEngine::lockWithPreloadedFrames(size_t preloadedFrames,
                                         int64_t nowUs) {
  playbackStartTimeUs_ = nowUs;
  samplesWritten_ = static_cast<int64_t>(preloadedFrames);
  playing_ = true;
}

int SyncEngine::steadyStateFrameAdjustment(int64_t shortM, int64_t miniM,
                                           int64_t age) const {
  // shortM < 0: actual play position is ahead of target (running early) -
  // slow down by duplicating a frame. shortM > 0: running late - catch up
  // by skipping one. See SyncResult::frameAdjustment's sign convention.
  if (shortM < -kShortOffsetUs && miniM < -kMiniOffsetUs && age < -kMiniOffsetUs) {
    return 1;
  }
  if (shortM > kShortOffsetUs && miniM > kMiniOffsetUs && age > kMiniOffsetUs) {
    return -1;
  }
  return 0;
}

}  // namespace snapclient
