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

void PlaybackStats::maybeLogSummary(RateLimiter& limiter, const char* logTag,
                                    int64_t now, size_t queueDepth,
                                    uint32_t i2sOverflow) {
  if (!limiter.due(now)) {
    return;
  }
  // skip/dup are deltas since the last log, not running totals - those
  // lose resolution over a multi-hour soak.
  const int64_t deltaSkip =
      static_cast<int64_t>(correctionsSkip - lastCorrectionsSkipLogged);
  const int64_t deltaDuplicate = static_cast<int64_t>(
      correctionsDuplicate - lastCorrectionsDuplicateLogged);
  lastCorrectionsSkipLogged = correctionsSkip;
  lastCorrectionsDuplicateLogged = correctionsDuplicate;
  BELL_LOG(info, logTag,
           "played={} dropped={} queued={} i2sSendQOverflow={} "
           "ageUs={} diffToServerUs={} netCorrection={} (skip={} "
           "dup={}) playingForS={}",
           chunksPlayed, chunksDropped, queueDepth - 1, i2sOverflow, lastAgeUs,
           lastDiffToServerUs, deltaSkip - deltaDuplicate, deltaSkip,
           deltaDuplicate, (now - lastResyncAtUs) / 1000000);
}

PlaybackPipeline::PlaybackPipeline(SnapcastClient& client, AudioSink& audioSink,
                                   const char* logTag)
    : logTag_(logTag),
      sync_(kQueueCapacity),
      audioSink_(audioSink),
      queue_(kQueueCapacity),
      pcmQueue_(kPcmQueueCapacity),
      decoder_(queue_, pcmQueue_, codecGeneration_, client),
      client_(client),
      serverSettingsLogLimiter_(1'000'000),
      dropLogLimiter_(30'000'000) {}

void PlaybackPipeline::applyDspSettings(DspFlow flow,
                                        const DspFilterParams& params) {
  dsp_.switchFlow(flow);
  dsp_.setParams(flow, params);
}

void PlaybackPipeline::onConnected() { sync_.reset(); }

void PlaybackPipeline::onServerSettings(const ServerSettings& s) {
  bufferMs_ = s.bufferMs;
  dacFixedLatencyMs_ = s.latencyMs;

  if (bufferMs_ != lastSyncBufferMs_) {
    lastSyncBufferMs_ = bufferMs_;
    sync_.onSettingsChanged(bufferMs_, static_cast<uint32_t>(sampleRate_));
  }
  dsp_.setVolume(static_cast<float>(s.volume) / 100.0f);
  audioSink_.setMuted(s.muted);

  // A UI volume slider sends one of these per tick while dragging - same
  // UART-stall reasoning as the drop log below for throttling it.
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
    queueFullDrops_++;
    droppedChunkFrames_.fetch_add(kFramesPerChunk, std::memory_order_relaxed);
  } else {
    QueuedChunk item;
    item.serverTimeUs = serverTimeUs;
    item.codec = codec;
    item.codecGen = codecGeneration_.load(std::memory_order_relaxed);
    // An allocation failure queues a silent placeholder (item.payload left
    // empty) instead of dropping the chunk outright - a dropped chunk
    // leaves a gap in the played frame count that the server's clock
    // doesn't have. See DecoderTask's silencePlayed log for how often this
    // actually results in audible silence.
    item.payload = acquireChunkBuffer(payload, len);
    if (!queue_.tryPush(std::move(item))) {
      queueFullDrops_++;
      droppedChunkFrames_.fetch_add(kFramesPerChunk, std::memory_order_relaxed);
    }
  }

  // Sustained overload logs every chunk otherwise - the blocking UART
  // write itself then becomes part of the overload, up to starving other
  // tasks long enough to trip the watchdog. 30s, not 1s: an occasional
  // single dropped chunk is normal jitter, not something worth a line
  // every second.
  if (dropLogLimiter_.due(nowUs())) {
    if (queueFullDrops_ > 0) {
      BELL_LOG(warn, logTag_, "dropped {} chunks (queue full) in the last 30s",
               queueFullDrops_);
      queueFullDrops_ = 0;
    }
  }
}

void PlaybackPipeline::consumeOnce(PlaybackStats& stats) {
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
      stats.chunksPlayed++;

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

      if (result.frameAdjustment < 0) {
        stats.correctionsSkip++;
      } else if (result.frameAdjustment > 0) {
        stats.correctionsDuplicate++;
      }

      // Tell SyncEngine the actual frames just written
      sync_.onFramesWritten(targetFrames);

      // Underruns advance the DAC's clock without a matching write() -
      // feed that into SyncEngine too. Only while already playing: a
      // resync's own search leaves the DMA idle on purpose, that's not
      // lost time.
      const uint32_t underrunFrames = audioSink_.underrunCompensationFrames();
      if (wasPlayingBeforeEval) {
        const uint32_t underrunDelta =
            underrunFrames - stats.lastUnderrunCompensationFrames;
        if (underrunDelta > 0) {
          sync_.onFramesWritten(underrunDelta);
        }
      }
      stats.lastUnderrunCompensationFrames = underrunFrames;

      // shortMedian_ isn't full on the very first chunk(s) after a
      // resync, so evaluate() reports ageUs=0/diffToServerUs=0 then -
      // harmless here since the drift log below only cares about the
      // steady-state trend over minutes/hours, not that transient.
      stats.lastAgeUs = result.ageUs;
      stats.lastDiffToServerUs = result.diffToServerUs;
    } else {
      stats.chunksDropped++;

      // Yield briefly so we don't starve other tasks
      std::this_thread::yield();

      if (!sync_.isPlaying()) {
        // Still resyncing after this drop - the rest of the backlog only
        // gets staler evaluated one at a time in FIFO order, since real
        // time keeps advancing while working through it. Jump straight
        // to the newest chunk instead, on both queues.
        stats.chunksDropped += pcmQueue_.drainToNewest(1);
        stats.chunksDropped += queue_.drainToNewest(1);
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
            stats.chunksDropped += dropped;
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

  // playing_ went false -> true: evaluate() just reset
  // playbackStartTimeUs_/samplesWritten_ (see SyncEngine.cpp), from a
  // hard resync or a fresh connection.
  if (!wasPlayingBeforeEval && sync_.isPlaying()) {
    stats.lastResyncAtUs = nowUs();
  }

  stats.maybeLogSummary(playedLogLimiter_, logTag_, nowUs(), queueDepth,
                        audioSink_.sendQueueOverflowCount());
}

}  // namespace snapclient
