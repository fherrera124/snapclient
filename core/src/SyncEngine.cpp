#include "snapclient/SyncEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include <bell/Logger.h>

namespace snapclient {

namespace {
const char* kLogTag = "SyncEngine";
}  // namespace

SyncEngine::SyncEngine(size_t queueCapacity)
    : timeFilter_(0.01, 0.0, 1.001, 0.75, 100, 2.0),
      shortMedian_(99),
      miniMedian_(19),
      queueCapacity_(queueCapacity) {}

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
                                size_t queueDepth, int32_t dacLatencyUs) {
  const int64_t diffToServer = timeFilter_.offsetAt(nowUs);
  const int64_t serverNowUs = nowUs + diffToServer;
  const int64_t bufferUs = int64_t{bufferMs_} * 1000;

  if (!playing_) {
    const int64_t age =
        serverNowUs - chunkServerTimeUs - bufferUs + dacLatencyUs;
    if (age < -kInitialSyncEarlyToleranceUs) {
      return {PlayDecision::WaitMore, -age, 0, false};
    }
    if (age > kInitialSyncLateToleranceUs) {
      // A backlog of these can be long enough that logging every one of
      // them (blocking UART write, unlike the aggregate log below) makes
      // draining it itself slower than chunks keep arriving, so the
      // backlog never shrinks.
      initialSyncDropCount_++;
      if (nowUs - lastInitialSyncDropLogUs_ >= 1000000) {
        lastInitialSyncDropLogUs_ = nowUs;
        BELL_LOG(warn, kLogTag,
                 "initial sync drop x{}: age={} diffToServer={} bufferUs={} "
                 "dacLatencyUs={}",
                 initialSyncDropCount_, age, diffToServer, bufferUs,
                 dacLatencyUs);
        initialSyncDropCount_ = 0;
      }
      return {PlayDecision::DropLate, 0, 0, false};
    }
    playbackStartTimeUs_ = nowUs;
    samplesWritten_ = 0;
    playing_ = true;
    return {PlayDecision::Play, 0, 0, false};
  }

  if (queueDepth == 0) {
    playing_ = false;
    shortMedian_.clear();
    miniMedian_.clear();
    return {PlayDecision::DropLate, 0, 0, true};
  }

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

  int frameAdjustment = 0;
  if (shortMedian_.full()) {
    frameAdjustment = steadyStateFrameAdjustment(
        shortMedian_.median(), miniMedian_.median(), age);
  }
  frameAdjustment = scaleFrameAdjustment(frameAdjustment, age, queueDepth);

  return {PlayDecision::Play, 0, frameAdjustment, false, age, diffToServer};
}

void SyncEngine::onFramesWritten(size_t frameCount) {
  samplesWritten_ += static_cast<int64_t>(frameCount);
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

int SyncEngine::scaleFrameAdjustment(int frameAdjustment, int64_t age,
                                     size_t queueDepth) const {
  if (frameAdjustment != 0) {
    // Scale the ±1 nudge by how far off we are. Tiers checked
    // widest-first, so only one applies.
    struct { int64_t ageUs; int factor; } kAmplifyTiers[] = {
        {12000, 6}, {8000, 4}, {4000, 2}};
    const int64_t absAge = std::abs(age);
    for (const auto& tier : kAmplifyTiers) {
      if (absAge > tier.ageUs) {
        frameAdjustment *= tier.factor;
        break;
      }
    }
  }

  // Skipped when already slowing down (frameAdjustment > 0), since
  // draining would fight that direction.
  if (frameAdjustment <= 0) {
    const size_t targetQueue = static_cast<size_t>(bufferMs_ / 20);
    struct { size_t margin; size_t capMargin; int adjust; } kQueueTiers[] = {
        {11, 2, -4}, {8, 4, -3}, {5, 6, -2}};
    for (const auto& tier : kQueueTiers) {
      const size_t threshold = std::min(targetQueue + tier.margin,
                                        queueCapacity_ - tier.capMargin);
      if (queueDepth > threshold) {
        // min(): keep whichever correction is larger.
        frameAdjustment = std::min(frameAdjustment, tier.adjust);
        break;
      }
    }
  }
  return frameAdjustment;
}

}  // namespace snapclient
