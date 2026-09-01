#include "snapclient/PlaybackPipeline.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include <bell/Logger.h>

#include "snapclient/DynamicResampler.h"

namespace snapclient {

namespace {

int64_t nowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

PlaybackPipeline::PlaybackPipeline(SnapcastClient& client, AudioSink& audioSink,
                                   const char* logTag)
    : logTag_(logTag),
      sync_(kQueueCapacity),
      audioSink_(audioSink),
      queue_(kQueueCapacity),
      pcmQueue_(kPcmQueueCapacity),
      decoder_(queue_, pcmQueue_, codecGeneration_, client),
      client_(client),
      serverSettingsLogLimiter_(1'000'000) {}

void PlaybackPipeline::applyDspSettings(DspFlow flow,
                                        const DspFilterParams& params) {
  dsp_.switchFlow(flow);
  dsp_.setParams(flow, params);
}

void PlaybackPipeline::onConnected() { sync_.reset(); }

void PlaybackPipeline::onServerSettings(const ServerSettings& s) {
  bufferMs_ = s.bufferMs;
  dacFixedLatencyMs_ = s.latencyMs;

  if (bufferMs_ != lastSyncBufferMs_ ||
      dacFixedLatencyMs_ != lastSyncDacLatencyMs_) {
    lastSyncBufferMs_ = bufferMs_;
    lastSyncDacLatencyMs_ = dacFixedLatencyMs_;
    sync_.onSettingsChanged(bufferMs_, static_cast<uint32_t>(sampleRate_));
  }
  dsp_.setVolume(static_cast<float>(s.volume) / 100.0f);
  audioSink_.setMuted(s.muted);

  // A UI volume slider sends one of these per tick while dragging - a
  // blocking UART write per tick would stall the audio hot path.
  if (serverSettingsLogLimiter_.due(nowUs())) {
    BELL_LOG(info, logTag_, "server settings: bufferMs={} volume={} muted={}",
             s.bufferMs, s.volume, s.muted);
  }
}

void PlaybackPipeline::onCodecReady(Codec /*codec*/,
                                    const bell::audio::Format& fmt) {
  sampleRate_ = fmt.getSampleRate();
  audioSink_.configure(fmt.getSampleRateValue());
  lastSyncBufferMs_ = bufferMs_;
  lastSyncDacLatencyMs_ = dacFixedLatencyMs_;
  sync_.onSettingsChanged(bufferMs_, fmt.getSampleRateValue());
  // Queued chunks predate this codec header and would otherwise decode
  // against the *new* decoder instance setupDecode() just recreated in
  // SnapcastClient - bump codecGeneration first so DecoderTask can also
  // recognize and drop anything already popped but not yet
  // decoded/published.
  codecGeneration_.fetch_add(1, std::memory_order_relaxed);
  queue_.clear();
  pcmQueue_.clear();
  BELL_LOG(info, logTag_, "codec ready: {} Hz, {} ch", fmt.getSampleRateValue(),
           fmt.getNumChannels());
}

void PlaybackPipeline::onTimeSample(int64_t offsetUs, int64_t maxErrorUs,
                                    int64_t t) {
  sync_.insertLatencySample(offsetUs, maxErrorUs, t);
  if (sync_.latencyReady()) {
    // A shorter interval keeps TimeFilter's offset fresher, but adds
    // network-thread load that can nudge queue depth past what the pool
    // backs with real encoded buffers - 1s keeps that load low.
    client_.setPingIntervalUs(1000000);
  }
}

void PlaybackPipeline::onAudioChunk(Codec codec, const std::byte* payload,
                                    size_t len, int64_t serverTimeUs) {
  if (!sync_.latencyReady()) {
    return;
  }

  const size_t effectiveQueueCeiling = std::min(
      kQueueCapacity, kQueueMemoryBudgetBytes / std::max<size_t>(len, 1));

  if (queue_.size() >= effectiveQueueCeiling) {
    droppedChunkFrames_.fetch_add(kFramesPerChunk, std::memory_order_relaxed);
  } else {
    QueuedChunk item;
    item.serverTimeUs = serverTimeUs;
    item.codec = codec;
    item.codecGen = codecGeneration_.load(std::memory_order_relaxed);
    // An allocation failure queues a silent placeholder (item.payload left
    // empty) instead of dropping the chunk outright - a dropped chunk
    // leaves a gap in the played frame count that the server's clock
    // doesn't have.
    item.payload = acquireChunkBuffer(payload, len);
    if (!queue_.tryPush(std::move(item))) {
      droppedChunkFrames_.fetch_add(kFramesPerChunk, std::memory_order_relaxed);
    }
  }
}

void PlaybackPipeline::consumeOnce() {
  // Applied here, not in onAudioChunk() - see droppedChunkFrames_'s comment.
  const size_t droppedFrames =
      droppedChunkFrames_.exchange(0, std::memory_order_relaxed);
  if (droppedFrames > 0 && sync_.isPlaying()) {
    sync_.onFramesWritten(droppedFrames);
  }

  DecodedChunk item;
  size_t queueDepth = 0;
  if (pcmQueue_.tryPop(item, 10)) {
    queueDepth = queue_.size() + pcmQueue_.size() + 1;
  }
  if (queueDepth == 0) {
    // tryPop() already waited up to 10ms internally on a miss - woken
    // early as soon as DecoderTask pushes something.
    return;
  }

  const std::byte* dspInput = item.pcm.data();
  size_t dspInputLen = item.pcm.size();
  size_t frames = dspInputLen / kBytesPerFrame;
  scratch_.resize(dspInputLen / sizeof(int16_t));
  dsp_.process(dspInput, dspInputLen, reinterpret_cast<std::byte*>(scratch_.data()),
              scratch_.size() * sizeof(int16_t), sampleRate_);

  const int32_t dacLatencyUs = static_cast<int32_t>(dacFixedLatencyMs_) * 1000;

  // Captured once before the retry loop below, not per-iteration:
  // WaitMore re-evaluates the same not-yet-playing chunk without ever
  // flipping playing_, so isPlaying() can't change mid-loop.
  const bool wasPlayingBeforeEval = sync_.isPlaying();

  for (;;) {
    const int64_t evalStartUs = nowUs();
    auto result = sync_.evaluate(item.serverTimeUs, evalStartUs, queueDepth,
                                 dacLatencyUs);

    if (result.decision == PlayDecision::WaitMore) {
      std::this_thread::sleep_for(std::chrono::microseconds(result.waitUs));
      continue;
    }
    if (result.decision == PlayDecision::Play) {
      // frameAdjustment is already scaled - see
      // SyncEngine::scaleFrameAdjustment(). Resampled across the whole
      // chunk, not a single-frame drop/duplicate at the edge, since the
      // magnitude can now exceed one frame.
      const size_t targetFrames = static_cast<size_t>(
          static_cast<int>(frames) + result.frameAdjustment);
      scratchResampled_.resize(targetFrames * 2);  // stereo
      DynamicResampler::process(reinterpret_cast<const int16_t*>(scratch_.data()),
                                frames, scratchResampled_.data(), targetFrames);

      audioSink_.write(reinterpret_cast<const std::byte*>(scratchResampled_.data()),
                       targetFrames * kBytesPerFrame);

      // Tell SyncEngine the actual frames just written
      sync_.onFramesWritten(targetFrames);

      // Underruns advance the DAC's clock without a matching write() -
      // feed that into SyncEngine too. Only while already playing: a
      // resync's own search leaves the DMA idle on purpose, that's not
      // lost time.
      const uint32_t underrunFrames = audioSink_.underrunCompensationFrames();
      if (wasPlayingBeforeEval) {
        const uint32_t underrunDelta =
            underrunFrames - lastUnderrunCompensationFrames_;
        if (underrunDelta > 0) {
          sync_.onFramesWritten(underrunDelta);
        }
      }
      lastUnderrunCompensationFrames_ = underrunFrames;
    } else {
      // Yield briefly so we don't starve other tasks
      std::this_thread::yield();

      if (!sync_.isPlaying() &&
          queue_.size() > static_cast<size_t>(bufferMs_ / 20)) {
        // Gated on an actual backlog: jumping to the newest chunk targets
        // a play time ~bufferMs_ away, and waiting that out starves I2S's
        // ~80ms DMA buffer badly enough to trigger the next hard resync
        // itself. A healthy-depth queue's front chunk is already close
        // to on-time.
        pcmQueue_.drainToNewest(1);
        queue_.drainToNewest(1);
      }
    }
    break;
  }

  // Frame-level correction can't shrink queue.size() by a whole chunk, so
  // backlog that never trips a hard resync otherwise never drains.
  // Compensates via onFramesWritten(), same as underrun does above.
  if (wasPlayingBeforeEval) {
    const int64_t queueExcessCheckNowUs = nowUs();
    const size_t targetQueue = static_cast<size_t>(bufferMs_ / 20);
    // Below ~220ms bufferMs, targetQueue-8 would underflow - disable
    // rather than let a wrapped size_t make the check silently inert.
    if (targetQueue > 11) {
      const size_t healthyQueueTarget = targetQueue - 8;
      const size_t queueExcessThreshold = healthyQueueTarget + 3;
      const size_t rawQueueSize = queue_.size();
      if (rawQueueSize > queueExcessThreshold) {
        if (queueExcessSinceUs_ == 0) {
          queueExcessSinceUs_ = queueExcessCheckNowUs;
        } else if (queueExcessCheckNowUs - queueExcessSinceUs_ >=
                  kQueueExcessSustainedUs) {
          const size_t dropped = queue_.drainToNewest(healthyQueueTarget);
          if (dropped > 0) {
            sync_.onFramesWritten(dropped * kFramesPerChunk);
            BELL_LOG(warn, logTag_,
                     "queue excess: raw queue {} -> {} (dropped {} chunks, "
                     "compensated {} frames)",
                     rawQueueSize, healthyQueueTarget, dropped,
                     dropped * kFramesPerChunk);
          }
          queueExcessSinceUs_ = 0;
        }
      } else {
        queueExcessSinceUs_ = 0;
      }
    }
  } else {
    // Explicit reset - a resync's own drain already clears this anyway.
    queueExcessSinceUs_ = 0;
  }
}

}  // namespace snapclient
