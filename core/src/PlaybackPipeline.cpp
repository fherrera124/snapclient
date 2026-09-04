#include "snapclient/PlaybackPipeline.h"

#include <algorithm>
#include <chrono>
#include <cmath>
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

const char* codecName(Codec codec) {
  switch (codec) {
    case Codec::Pcm:
      return "PCM";
    case Codec::Opus:
      return "Opus";
    case Codec::Flac:
      return "FLAC";
    case Codec::None:
      return "none";
  }
  return "unknown";
}

}  // namespace

PlaybackPipeline::PlaybackPipeline(SnapcastClient& client, AudioSink& audioSink,
                                   PrecisionWaiter& waiter, const char* logTag)
    : logTag_(logTag),
      audioSink_(audioSink),
      waiter_(waiter),
      queue_(kQueueCapacity),
      pcmQueue_(kPcmQueueCapacity),
      decoder_(queue_, pcmQueue_, codecGeneration_, client, dsp_,
              sampleRateHz_, samplesPerChunkHint_),
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
    applyQueueCapacity();
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

void PlaybackPipeline::onCodecReady(Codec codec,
                                    const bell::audio::Format& fmt) {
  sampleRate_ = fmt.getSampleRate();
  sampleRateHz_.store(fmt.getSampleRateValue(), std::memory_order_relaxed);
  const uint32_t expectedSamplesPerChunk = client_.expectedSamplesPerChunk();
  samplesPerChunkHint_.store(expectedSamplesPerChunk,
                             std::memory_order_relaxed);
  appliedChunkFrames_.store(expectedSamplesPerChunk,
                            std::memory_order_relaxed);
  audioSink_.configure(fmt.getSampleRateValue(), expectedSamplesPerChunk);
  // Reserved once so prepareForOutput()'s resize() never allocates on the
  // audio path; +8 covers frameAdjustment's per-chunk swing.
  scratchResampled_.reserve((expectedSamplesPerChunk + 8) * 2);
  lastSyncBufferMs_ = bufferMs_;
  lastSyncDacLatencyMs_ = dacFixedLatencyMs_;
  sync_.onSettingsChanged(bufferMs_, fmt.getSampleRateValue());
  applyQueueCapacity();
  // Queued chunks predate this codec header and would otherwise decode
  // against the *new* decoder instance setupDecode() just recreated in
  // SnapcastClient - bump codecGeneration first so DecoderTask can also
  // recognize and drop anything already popped but not yet
  // decoded/published.
  codecGeneration_.fetch_add(1, std::memory_order_relaxed);
  queue_.clear();
  pcmQueue_.clear();
  pendingChunk_.reset();
  BELL_LOG(info, logTag_, "codec ready: {} {} Hz, {}-bit, {} ch",
           codecName(codec), fmt.getSampleRateValue(),
           bell::audio::getSampleSizeInBytes(fmt.getSampleFormat()) * 8,
           fmt.getNumChannels());
}

void PlaybackPipeline::applyQueueCapacity() {
  const uint32_t sr = sampleRateHz_.load(std::memory_order_relaxed);
  if (bufferMs_ <= 0 || sr == 0) {
    return;
  }
  const uint32_t samplesPerChunk =
      samplesPerChunkHint_.load(std::memory_order_relaxed);
  const double chunkDurationMs = samplesPerChunk * 1000.0 / sr;
  const size_t entries =
      static_cast<size_t>(std::ceil(bufferMs_ / chunkDurationMs));
  const size_t capacity = std::max<size_t>(entries, 1);
  queue_.setCapacity(capacity);
  BELL_LOG(info, logTag_, "queue capacity: {} chunks (bufferMs={} sr={})",
           capacity, bufferMs_, sr);
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

void PlaybackPipeline::onAudioChunk(Codec codec, ChunkBuffer payload,
                                    int64_t serverTimeUs) {
  if (!sync_.latencyReady()) {
    return;
  }

  // Drops stay unaccounted for: the gap reaches SyncEngine as a chunk
  // timestamp that advanced with no frames written.
  if (queue_.size() >= queue_.capacity()) {
    return;
  }

  QueuedChunk item;
  item.serverTimeUs = serverTimeUs;
  item.codec = codec;
  item.codecGen = codecGeneration_.load(std::memory_order_relaxed);
  // A falsy payload (SnapcastClient couldn't allocate for it) is still
  // queued - DecoderTask turns it into silence of the right length.
  item.payload = std::move(payload);
  queue_.tryPush(std::move(item));
}

size_t PlaybackPipeline::prepareForOutput(const std::byte* pcm, size_t len,
                                         int frameAdjustment) {
  const size_t frames = len / kBytesPerFrame;

  // Resampled across the whole chunk, not a single-frame drop/duplicate at
  // the edge, since the magnitude can now exceed one frame.
  const size_t targetFrames =
      static_cast<size_t>(static_cast<int>(frames) + frameAdjustment);
  scratchResampled_.resize(targetFrames * 2);  // stereo
  if (pcm == nullptr) {
    std::fill(scratchResampled_.begin(), scratchResampled_.end(), 0);
    return targetFrames;
  }
  DynamicResampler::process(reinterpret_cast<const int16_t*>(pcm), frames,
                            scratchResampled_.data(), targetFrames);
  return targetFrames;
}

// lockOntoChunk() must preload the whole ring before its armed deadline,
// and decode runs near real time - hence the ring duration plus margin.
int64_t PlaybackPipeline::minLockLeadUs() const {
  const uint32_t sr = sampleRateHz_.load(std::memory_order_relaxed);
  if (sr == 0) {
    return 0;
  }
  return int64_t{audioSink_.ringCapacityFrames()} * 1'000'000 * 3 / (sr * 2);
}

void PlaybackPipeline::lockOntoChunk(DecodedChunk firstItem, int64_t waitUs) {
  waiter_.arm(waitUs);
  audioSink_.disable();

  size_t preloadedFrames = 0;
  DecodedChunk item = std::move(firstItem);
  size_t offsetFrames = 0;
  for (;;) {
    const size_t itemFrames = item.frames() - offsetFrames;
    const size_t frames = prepareForOutput(item.dataAt(offsetFrames),
                                           itemFrames * kBytesPerFrame,
                                           /*frameAdjustment=*/0);
    const size_t requestedBytes = frames * kBytesPerFrame;
    const size_t accepted = audioSink_.preload(
        reinterpret_cast<const std::byte*>(scratchResampled_.data()),
        requestedBytes);
    const size_t acceptedFrames = accepted / kBytesPerFrame;
    preloadedFrames += acceptedFrames;

    if (accepted < requestedBytes) {
      // Output filled up mid-chunk - keep the exact unaccepted remainder
      // for consumeOnce() to finish normally next, instead of dropping it.
      if (acceptedFrames < itemFrames) {
        pendingChunk_ =
            PendingChunk{std::move(item), offsetFrames + acceptedFrames};
      }
      break;
    }
    if (!pcmQueue_.tryPop(item, 2000)) {
      break;  // nothing more decoded yet - wait out the rest of the alarm
    }
    offsetFrames = 0;
  }

  waiter_.block();
  audioSink_.enable();
  sync_.lockWithPreloadedFrames(preloadedFrames, nowUs());
  BELL_LOG(info, logTag_, "locked: preloaded {} frames, waited {}us",
           preloadedFrames, waitUs);
}

void PlaybackPipeline::consumeOnce() {
  // Re-size both once DecoderTask reports a chunk size onCodecReady() did
  // not have. Guarded because applyQueueCapacity() is not a no-op.
  const uint32_t samplesPerChunk =
      samplesPerChunkHint_.load(std::memory_order_relaxed);
  if (samplesPerChunk != appliedChunkFrames_.load(std::memory_order_relaxed)) {
    audioSink_.configure(sampleRateHz_.load(std::memory_order_relaxed),
                         samplesPerChunk);
    applyQueueCapacity();
    appliedChunkFrames_.store(samplesPerChunk, std::memory_order_relaxed);
  }

  DecodedChunk item;
  size_t offsetFrames = 0;
  size_t queueDepth = 0;
  if (pendingChunk_) {
    item = std::move(pendingChunk_->chunk);
    offsetFrames = pendingChunk_->framesConsumed;
    pendingChunk_.reset();
    queueDepth = queue_.size() + pcmQueue_.size() + 1;
  } else if (pcmQueue_.tryPop(item, 10)) {
    queueDepth = queue_.size() + pcmQueue_.size() + 1;
  }
  if (queueDepth == 0) {
    // tryPop() already waited up to 10ms internally on a miss - woken
    // early as soon as DecoderTask pushes something.
    return;
  }

  // A resumed PendingChunk's timestamp/PCM start partway through the
  // original chunk - offsetFrames is 0 for a freshly popped item, making
  // both of these a no-op slice.
  const int64_t itemServerTimeUs =
      item.serverTimeUs +
      static_cast<int64_t>(offsetFrames) * 1'000'000LL /
          sampleRateHz_.load(std::memory_order_relaxed);
  const std::byte* pcm = item.dataAt(offsetFrames);
  const size_t pcmLen = (item.frames() - offsetFrames) * kBytesPerFrame;

  const int32_t dacLatencyUs = static_cast<int32_t>(dacFixedLatencyMs_) * 1000;

  auto result = sync_.evaluate(itemServerTimeUs, nowUs(), queueDepth,
                               dacLatencyUs, pcmLen / kBytesPerFrame,
                               minLockLeadUs());

  if (result.decision == PlayDecision::WaitMore) {
    lockOntoChunk(std::move(item), result.waitUs);
    return;
  }

  if (result.decision == PlayDecision::Play) {
    const size_t targetFrames =
        prepareForOutput(pcm, pcmLen, result.frameAdjustment);
    audioSink_.write(reinterpret_cast<const std::byte*>(scratchResampled_.data()),
                     targetFrames * kBytesPerFrame);

    // Tell SyncEngine the actual frames just written
    sync_.onFramesWritten(targetFrames);
  } else {
    // DropLate. During the initial-sync search, chunksToSkip more queued
    // chunks are already stale too - drop them all now in one pass. 0
    // for the hard-resync DropLate cases (queue starvation, threshold
    // exceeded).
    DecodedChunk discard;
    for (int i = 0; i < result.chunksToSkip; i++) {
      if (!pcmQueue_.tryPop(discard, 1)) {
        break;
      }
    }
    std::this_thread::yield();
  }
}

}  // namespace snapclient
