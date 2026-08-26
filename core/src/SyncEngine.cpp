#include "snapclient/SyncEngine.h"

#include <cmath>

#include <bell/Logger.h>

namespace snapclient {

namespace {
const char* kLogTag = "SyncEngine";
}  // namespace

SyncEngine::SyncEngine()
    : timeFilter_(0.01, 0.0, 1.001, 0.75, 100, 2.0),
      shortMedian_(99),
      miniMedian_(19) {}

void SyncEngine::onSettingsChanged(int32_t bufferMs, int32_t dacLatencyMs,
                                   uint32_t sampleRate) {
  bufferMs_ = bufferMs;
  dacLatencyMs_ = dacLatencyMs;
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
}

SyncResult SyncEngine::evaluate(int64_t chunkServerTimeUs, int64_t nowUs,
                                size_t queueDepth) {
  const int64_t diffToServer = timeFilter_.offsetAt(nowUs);
  const int64_t serverNowUs = nowUs + diffToServer;
  const int64_t bufferUs = int64_t{bufferMs_} * 1000;
  const int64_t dacLatencyUs = int64_t{dacLatencyMs_} * 1000;

  if (!playing_) {
    const int64_t age =
        serverNowUs - chunkServerTimeUs - bufferUs + dacLatencyUs;
    if (age < -kInitialSyncToleranceUs) {
      return {PlayDecision::WaitMore, -age, 0, false};
    }
    if (age > kInitialSyncToleranceUs) {
      BELL_LOG(warn, kLogTag,
               "initial sync drop: age={} diffToServer={} bufferUs={} "
               "dacLatencyUs={}",
               age, diffToServer, bufferUs, dacLatencyUs);
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
             "diffToServer={}",
             age, shortMedian_.median(), miniMedian_.median(), diffToServer);
    playing_ = false;
    shortMedian_.clear();
    miniMedian_.clear();
    return {PlayDecision::DropLate, 0, 0, true};
  }

  int frameAdjustment = 0;
  if (shortMedian_.full()) {
    const int64_t shortM = shortMedian_.median();
    const int64_t miniM = miniMedian_.median();
    if (shortM < -kShortOffsetUs && miniM < -kMiniOffsetUs && age < -kMiniOffsetUs) {
      frameAdjustment = 1;
    } else if (shortM > kShortOffsetUs && miniM > kMiniOffsetUs && age > kMiniOffsetUs) {
      frameAdjustment = -1;
    }
  }

  return {PlayDecision::Play, 0, frameAdjustment, false};
}

void SyncEngine::onFramesWritten(size_t frameCount) {
  samplesWritten_ += static_cast<int64_t>(frameCount);
}

}  // namespace snapclient
