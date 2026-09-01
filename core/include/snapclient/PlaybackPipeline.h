#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <bell/audio/Common.h>

#include "snapclient/AudioSink.h"
#include "snapclient/BoundedQueue.h"
#include "snapclient/ChunkBuffer.h"
#include "snapclient/DecoderTask.h"
#include "snapclient/DspProcessor.h"
#include "snapclient/SnapcastClient.h"
#include "snapclient/SyncEngine.h"

namespace snapclient {

class RateLimiter {
 public:
  explicit RateLimiter(int64_t intervalUs) : intervalUs_(intervalUs) {}
  bool due(int64_t nowUs) {
    if (nowUs - lastUs_ < intervalUs_) {
      return false;
    }
    lastUs_ = nowUs;
    return true;
  }

 private:
  int64_t intervalUs_;
  int64_t lastUs_ = 0;
};

// The five onXxx methods run on SnapcastClient's own thread; consumeOnce()
// runs on the caller's - plain reads/writes across that boundary, no
// synchronization.
class PlaybackPipeline {
 public:
  PlaybackPipeline(SnapcastClient& client, AudioSink& audioSink,
                   const char* logTag);

  void applyDspSettings(DspFlow flow, const DspFilterParams& params);
  void onConnected();
  void onServerSettings(const ServerSettings& s);
  void onCodecReady(Codec codec, const bell::audio::Format& fmt);
  void onTimeSample(int64_t offsetUs, int64_t maxErrorUs, int64_t t);
  void onAudioChunk(Codec codec, const std::byte* payload, size_t len,
                    int64_t serverTimeUs);

  // Pops and plays (or drops) one chunk, or returns immediately if none
  // are ready yet. Call in a tight loop.
  void consumeOnce();

 private:
  // Bounds the network-to-playback queue. A chunk acquireChunkBuffer()
  // couldn't get memory for still carries useful timing (it falls back to
  // the accounted-silence path in onAudioChunk below) - high enough to
  // absorb ordinary network jitter without starving the queue into
  // unaccounted drops.
  static constexpr size_t kQueueCapacity = 40;
  // A count-based cap alone assumes Opus-sized chunks (RFC 6716 caps a
  // frame at 1275B) - a board with no PSRAM can't hold kQueueCapacity
  // chunks of uncompressed Pcm (3840B each) in internal heap.
  static constexpr size_t kQueueMemoryBudgetBytes = 16 * 1024;
  // Decode (~12ms avg) is slower than dsp+i2s-write (~6.5-7.5ms combined),
  // so 2 slots is enough for one-chunk-ahead overlap between DecoderTask
  // and the consumer; bump if soak testing shows the consumer stalling
  // here waiting on decode.
  static constexpr size_t kPcmQueueCapacity = 2;
  // Longer than shortMedian_'s ~2s window, so one drain's outliers
  // clear it before the next drain's debounce could complete.
  static constexpr int64_t kQueueExcessSustainedUs = 3'000'000;  // 3s

  const char* logTag_;

  SyncEngine sync_;
  DspProcessor dsp_;
  AudioSink& audioSink_;
  BoundedQueue<QueuedChunk> queue_;
  BoundedQueue<DecodedChunk> pcmQueue_;
  std::atomic<uint32_t> codecGeneration_{0};
  DecoderTask decoder_;
  SnapcastClient& client_;

  bell::audio::SampleRate sampleRate_ = bell::audio::SampleRate::SR_44100HZ;
  int32_t bufferMs_ = 0;
  // Snapcast's per-client "latency" setting, which nothing on this end
  // can measure directly.
  int32_t dacFixedLatencyMs_ = 0;
  // Tracks what SyncEngine was last told, so a ServerSettings message
  // that only changed volume/mute (bundled together in the same message
  // by the protocol) doesn't also force a resync - sync_.onSettingsChanged
  // drops playing_ back to the initial-sync state, which costs several
  // seconds to recover from and has nothing to do with volume.
  // dacFixedLatencyMs_ needs the same tracking - it's baked into the same
  // age formula as bufferMs_, so changing it mid-stream is just as
  // disruptive.
  int32_t lastSyncBufferMs_ = 0;
  int32_t lastSyncDacLatencyMs_ = 0;

  RateLimiter serverSettingsLogLimiter_;
  // A chunk dropped in onAudioChunk() never calls sync_.onFramesWritten(),
  // so sync_'s clock would fall behind real elapsed server-time - deferred
  // here since onAudioChunk() runs on a different thread than sync_'s.
  std::atomic<size_t> droppedChunkFrames_{0};

  std::vector<int16_t> scratch_;
  std::vector<int16_t> scratchResampled_;
  int64_t queueExcessSinceUs_ = 0;
  uint32_t lastUnderrunCompensationFrames_ = 0;
};

}  // namespace snapclient
