#include "snapclient/PlaybackPipeline.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <new>
#include <thread>

#include <bell/Logger.h>

#ifdef ESP_PLATFORM
#include <bell/utils/EspMemoryResource.h>
#include "esp_heap_caps.h"
#endif

#include "snapclient/FrameSplicer.h"

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

// The DMA never reads it, so it need not be internal.
std::pmr::memory_resource* outputBufferResource() {
#ifdef ESP_PLATFORM
  static bell::utils::EspFallbackMemoryResource<
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT>
      psramThenInternal;
  if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) {
    return &psramThenInternal;
  }
#endif
  return std::pmr::get_default_resource();
}

}  // namespace

PlaybackPipeline::PlaybackPipeline(SnapcastClient& client, AudioSink& audioSink,
                                   PrecisionWaiter& waiter, const char* logTag)
    : logTag_(logTag),
      audioSink_(audioSink),
      waiter_(waiter),
      queue_(kQueueCapacity),
      pcmQueue_(kPcmQueueCapacity),
      decoder_(queue_, pcmQueue_, codecGeneration_, client,
              samplesPerChunkHint_),
      client_(client),
      queueFullLogLimiter_(kLossLogIntervalUs),
      starvationLogLimiter_(kLossLogIntervalUs),
      dspFailureLogLimiter_(kLossLogIntervalUs),
      scratchOutput_(kOutputBlockFrames * 2, outputBufferResource()) {}

void PlaybackPipeline::applyDspSettings(DspFlow flow,
                                        const DspFilterParams& params) {
  {
    std::lock_guard<std::mutex> lock(dspMutex_);
    dspFlow_ = flow;
    dspParams_ = params;
  }
  routeEq();
}

void PlaybackPipeline::routeEq() {
  std::lock_guard<std::mutex> lock(dspMutex_);
  const DspStages stages = dspFlowStages(dspFlow_, dspParams_);
  const bool inSink = stages.linked && audioSink_.setEq(stages.left);
  if (!stages.linked) {
    audioSink_.setEq({});
  }
  if (!stages.left.empty()) {
    BELL_LOG(info, logTag_, "EQ runs in {}", inSink ? "the DAC" : "software");
  }
  dsp_.setParams(dspFlow_, dspParams_);
  dsp_.switchFlow(inSink ? DspFlow::Stereo : dspFlow_);
}

void PlaybackPipeline::setDownmixMono(bool downmix) {
  dsp_.setDownmixMono(downmix);
}

void PlaybackPipeline::onConnected() { sync_.reset(); }

void PlaybackPipeline::onServerSettings(const ServerSettings& s) {
  bufferMs_ = s.bufferMs;
  dacFixedLatencyMs_ = s.latencyMs;

  const bool syncChanged = bufferMs_ != lastSyncBufferMs_ ||
                           dacFixedLatencyMs_ != lastSyncDacLatencyMs_;
  if (syncChanged) {
    lastSyncBufferMs_ = bufferMs_;
    lastSyncDacLatencyMs_ = dacFixedLatencyMs_;
    sync_.onSettingsChanged(bufferMs_, static_cast<uint32_t>(sampleRate_));
    applyQueueCapacity();
  }
  if (syncChanged || s.volume != lastVolume_ || s.muted != lastMuted_) {
    lastVolume_ = s.volume;
    lastMuted_ = s.muted;
    // Runs on SnapcastClient's network task, where a blocking write
    // delays chunk reception.
    settingsLogDueUs_ = nowUs() + kSettingsLogSettleUs;
  }
  const float volume = static_cast<float>(s.volume) / 100.0f;
  dsp_.setVolume(audioSink_.setVolume(volume) ? 1.0f : volume);
  audioSink_.setMuted(s.muted);
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
  routeEq();
  // Reserved once so prepareForOutput()'s resize() never allocates on the
  // audio path; +8 covers frameAdjustment's per-chunk swing.
  scratchResampled_.reserve((expectedSamplesPerChunk + 8) * 2);
  const size_t requestedSlots = kPcmQueueCapacity + kPcmBuffersInFlight;
  const size_t poolSlots = configureChunkPool(
      expectedSamplesPerChunk * kBytesPerFrame, requestedSlots);
  const ChunkPoolTiers tiers = chunkPoolTierCounts();
  BELL_LOG(info, logTag_,
           "chunk pool: {}/{} slots of {} bytes - {} psram, {} iram, {} dram",
           poolSlots, requestedSlots, expectedSamplesPerChunk * kBytesPerFrame,
           tiers.psram, tiers.iram, tiers.dram);
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
    ++queueFullDrops_;
    if (queueFullLogLimiter_.due(nowUs())) {
      // A blocked re-lock backpressures the decoder through pcmQueue_,
      // so a full queue then is not decode's fault.
      BELL_LOG(warn, logTag_,
               "audio lost: chunk queue full at {} ({} chunks so far, {})",
               queue_.capacity(), queueFullDrops_,
               sync_.isPlaying() ? "decode is behind" : "waiting to re-lock");
    }
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

  const size_t targetFrames =
      static_cast<size_t>(static_cast<int>(frames) + frameAdjustment);
  scratchResampled_.resize(targetFrames * 2);  // stereo
  if (pcm == nullptr) {
    std::fill(scratchResampled_.begin(), scratchResampled_.end(), 0);
    return targetFrames;
  }
  FrameSplicer::process(reinterpret_cast<const int16_t*>(pcm), frames,
                        scratchResampled_.data(), targetFrames);
  return targetFrames;
}

size_t PlaybackPipeline::output(size_t frames, bool preload) {
  size_t done = 0;
  while (done < frames) {
    const size_t n = std::min(frames - done, kOutputBlockFrames);
    processBlock(scratchResampled_.data() + done * 2, n);
    const auto* block =
        reinterpret_cast<const std::byte*>(scratchOutput_.data());
    const size_t len = n * kSinkBytesPerFrame;
    if (!preload) {
      audioSink_.write(block, len);
      done += n;
      continue;
    }
    const size_t accepted = audioSink_.preload(block, len) / kSinkBytesPerFrame;
    done += accepted;
    if (accepted < n) {
      break;
    }
  }
  return done;
}

void PlaybackPipeline::processBlock(const int16_t* in, size_t frames) {
  try {
    dsp_.process(in, scratchOutput_.data(), frames,
                 static_cast<bell::audio::SampleRate>(
                     sampleRateHz_.load(std::memory_order_relaxed)));
  } catch (const std::bad_alloc&) {
    // Playing without the DSP beats losing the device to a slot the engine
    // could not size.
    for (size_t i = 0; i < frames * 2; i++) {
      scratchOutput_[i] = int32_t{in[i]} * 65536;
    }
    if (dspFailureLogLimiter_.due(nowUs())) {
      BELL_LOG(error, logTag_,
               "dsp out of memory - passing audio through unprocessed");
    }
  }
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
    // Only what fits goes through the DSP, so the remainder later resumes
    // from the filter state this left.
    const size_t ring = audioSink_.ringCapacityFrames();
    const size_t room = ring > preloadedFrames ? ring - preloadedFrames : 0;
    const size_t itemFrames = item.frames() - offsetFrames;
    const size_t frames = prepareForOutput(
        item.dataAt(offsetFrames), std::min(itemFrames, room) * kBytesPerFrame,
        /*frameAdjustment=*/0);
    const size_t acceptedFrames = output(frames, /*preload=*/true);
    preloadedFrames += acceptedFrames;

    if (acceptedFrames < itemFrames) {
      // Output filled up mid-chunk - keep the exact unaccepted remainder
      // for consumeOnce() to finish normally next, instead of dropping it.
      pendingChunk_ =
          PendingChunk{std::move(item), offsetFrames + acceptedFrames};
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

void PlaybackPipeline::flushSettingsLog() {
  if (settingsLogDueUs_ == 0 || nowUs() < settingsLogDueUs_) {
    return;
  }
  settingsLogDueUs_ = 0;
  BELL_LOG(info, logTag_,
           "server settings: bufferMs={} latencyMs={} volume={} muted={}",
           bufferMs_, dacFixedLatencyMs_, lastVolume_, lastMuted_);
}

void PlaybackPipeline::consumeOnce() {
  flushSettingsLog();
  // Re-size both once DecoderTask reports a chunk size onCodecReady() did
  // not have. Guarded because applyQueueCapacity() is not a no-op.
  const uint32_t samplesPerChunk =
      samplesPerChunkHint_.load(std::memory_order_relaxed);
  if (samplesPerChunk != appliedChunkFrames_.load(std::memory_order_relaxed)) {
    audioSink_.configure(sampleRateHz_.load(std::memory_order_relaxed),
                         samplesPerChunk);
    routeEq();
    applyQueueCapacity();
    appliedChunkFrames_.store(samplesPerChunk, std::memory_order_relaxed);
  }

  DecodedChunk item;
  size_t offsetFrames = 0;
  bool haveChunk = false;
  if (pendingChunk_) {
    item = std::move(pendingChunk_->chunk);
    offsetFrames = pendingChunk_->framesConsumed;
    pendingChunk_.reset();
    haveChunk = true;
  } else if (pcmQueue_.tryPop(item, 10)) {
    haveChunk = true;
  }
  if (!haveChunk) {
    // tryPop() already waited up to 10ms internally on a miss - woken
    // early as soon as DecoderTask pushes something.
    if (sync_.isPlaying()) {
      ++starvedPolls_;
      const int64_t now = nowUs();
      if (starvedSinceUs_ == 0) {
        starvedSinceUs_ = now;
      } else if (now - starvedSinceUs_ > int64_t{bufferMs_} * 1000) {
        sync_.onPlaybackGap();
        starvedSinceUs_ = 0;
      }
      if (starvationLogLimiter_.due(now)) {
        BELL_LOG(warn, logTag_,
                 "audio starved: nothing decoded for 10ms ({} times, {} "
                 "chunks waiting) - {} is behind",
                 starvedPolls_, queue_.size(),
                 queue_.size() == 0 ? "the network" : "decode");
      }
    }
    return;
  }
  starvedSinceUs_ = 0;

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

  auto result = sync_.evaluate(itemServerTimeUs, nowUs(), dacLatencyUs,
                               pcmLen / kBytesPerFrame, minLockLeadUs(),
                               audioSink_.clockDriftUs());

  if (result.decision == PlayDecision::WaitMore) {
    lockOntoChunk(std::move(item), result.waitUs);
    return;
  }

  if (result.decision == PlayDecision::Play) {
    const size_t targetFrames =
        prepareForOutput(pcm, pcmLen, result.frameAdjustment);
    output(targetFrames, /*preload=*/false);

    // Tell SyncEngine the actual frames just written
    sync_.onFramesWritten(targetFrames);
  } else {
    // DropLate. chunksToSkip more queued chunks are already stale too -
    // drop them in one pass; 0 when the hard-resync threshold fired.
    // The count comes out of both queues: pcmQueue_ holds only what decode
    // has run ahead by, and the rest of the backlog is in queue_.
    int remaining = result.chunksToSkip;
    DecodedChunk decodedDiscard;
    while (remaining > 0 && pcmQueue_.tryPop(decodedDiscard, 0)) {
      --remaining;
    }
    QueuedChunk encodedDiscard;
    while (remaining > 0 && queue_.tryPop(encodedDiscard, 0)) {
      --remaining;
    }
    std::this_thread::yield();
  }
}

}  // namespace snapclient
