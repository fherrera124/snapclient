#pragma once

#include <cstddef>
#include <cstdint>

#include "snapclient/SlidingMedian.h"
#include "snapclient/TimeFilter.h"

namespace snapclient {

enum class PlayDecision { WaitMore, DropLate, Play };

struct SyncResult {
  PlayDecision decision = PlayDecision::WaitMore;
  int64_t waitUs = 0;
  // Negative skips |frameAdjustment| frames (catch up), positive duplicates
  // that many (slow down), 0 = none.
  int frameAdjustment = 0;
  bool hardResync = false;
  // Meaningful only when decision == Play, past the first chunk of a
  // resync. Set to evaluate()'s own values so callers don't recompute them.
  int64_t ageUs = 0;
  int64_t diffToServerUs = 0;
  // Meaningful only when decision == DropLate and !hardResync (the
  // initial-sync catch-up case) - how many more queued chunks are also
  // already stale and should be discarded before evaluating again.
  int chunksToSkip = 0;
};

// Decides when a chunk should play and by how much to nudge playback to
// stay in sync with the server clock. Hardware-agnostic: the caller owns
// the actual audio output and reports back how many frames it wrote.
class SyncEngine {
 public:
  SyncEngine();

  void onSettingsChanged(int32_t bufferMs, uint32_t sampleRate);

  // offsetUs/maxErrorUs/nowUs all in microseconds - from a TIME round trip.
  void insertLatencySample(int64_t offsetUs, int64_t maxErrorUs,
                           int64_t nowUs);
  bool latencyReady() const;

  // Call once per chunk, before writing it. dacLatencyUs is a
  // parameter, not settings-cached state, because it can legitimately
  // change every call (e.g. an output sink reporting real, current DMA
  // ring occupancy) - caching it would mean either recomputing
  // onSettingsChanged() every chunk (which also resets playing_/medians,
  // wrongly treating a routine latency update as a resync-worthy event)
  // or silently evaluating against a stale value. chunkFrames is this
  // chunk's actual frame count, not a fixed assumption - it varies by
  // codec (and, for PCM, by the server's chunk_ms setting).
  // minLockLeadUs is how far ahead a chunk must be due before this
  // commits to it; 0 accepts any chunk not already late.
  SyncResult evaluate(int64_t chunkServerTimeUs, int64_t nowUs,
                      int32_t dacLatencyUs, size_t chunkFrames,
                      int64_t minLockLeadUs);

  // Call after writing a chunk (or after a WaitMore/DropLate outcome,
  // frameCount=0), so the virtual playback clock stays accurate.
  void onFramesWritten(size_t frameCount);

  // Call once, after independently handling a WaitMore decision (arming a
  // precise wait, preloading real audio into the output, blocking for it,
  // then enabling output) - preloadedFrames is exactly how much of that
  // audio actually reached the output, becoming the starting point for the
  // virtual playback clock instead of 0.
  void lockWithPreloadedFrames(size_t preloadedFrames, int64_t nowUs);

  void reset();

  // True once evaluate() has found a chunk to start from and hasn't since
  // fallen back to resyncing (queue starvation or a hard resync).
  bool isPlaying() const { return playing_; }

 private:
  static constexpr uint32_t kLatencyFilterFull = 29;
  static constexpr int64_t kHardResyncThresholdUs = 2000;
  static constexpr int64_t kShortOffsetUs = 128;
  static constexpr int64_t kMiniOffsetUs = 64;

  // -1 catches up, +1 slows down, 0 if the three signals disagree or the
  // medians aren't full. Fixed magnitude.
  int steadyStateFrameAdjustment(int64_t shortM, int64_t miniM,
                                 int64_t age) const;

  TimeFilter timeFilter_;
  SlidingMedian<int64_t> shortMedian_;
  SlidingMedian<int64_t> miniMedian_;

  int32_t bufferMs_ = 0;
  uint32_t sampleRate_ = 44100;

  bool playing_ = false;
  int64_t playbackStartTimeUs_ = 0;
  int64_t samplesWritten_ = 0;
};

}  // namespace snapclient
