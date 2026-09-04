#include "snapclient/DecoderTask.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

#include <bell/Logger.h>
#include <tcb/span.hpp>

#include "snapclient/SnapcastClient.h"

namespace snapclient {

namespace {
constexpr int64_t kAllocDropYieldMs = 10;

int64_t nowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

const std::vector<int16_t> kSilenceChunk(kOpusSamplesPerChunk * 2, 0);
}  // namespace

DecoderTask::DecoderTask(BoundedQueue<QueuedChunk>& rawQueue,
                         BoundedQueue<DecodedChunk>& pcmQueue,
                         std::atomic<uint32_t>& codecGeneration,
                         SnapcastClient& client, DspProcessor& dsp,
                         std::atomic<uint32_t>& sampleRateHz,
                         std::atomic<uint32_t>& samplesPerChunkHint,
                         std::atomic<size_t>& droppedChunkFrames)
    // Above SnapcastClient's priority (4) - pcmQueue_'s 2-slot margin is
    // far thinner than queue_'s (tens of chunks), so decode losing the CPU
    // race matters more than network receive doing so. CoreAny, not pinned
    // to the consumer's Core1 - if the consumer ever spins in a tight loop
    // (e.g. a resync storm), a same-core lower-priority task never gets to
    // run at all, starving decode entirely and making the storm
    // self-sustaining.
    : bell::Task("decoder", 16 * 1024, /*espPriority=*/10,
                bell::TaskCore::CoreAny, /*espStackOnPsram=*/false),
      rawQueue_(rawQueue),
      pcmQueue_(pcmQueue),
      codecGeneration_(codecGeneration),
      client_(client),
      dsp_(dsp),
      sampleRateHz_(sampleRateHz),
      samplesPerChunkHint_(samplesPerChunkHint),
      droppedChunkFrames_(droppedChunkFrames) {
  startTask();
}

void DecoderTask::runTask() {
  int64_t lastAllocDropYieldUs = 0;

  while (true) {
    QueuedChunk item;
    if (!rawQueue_.tryPop(item, 10)) {
      continue;
    }

    if (item.codecGen != codecGeneration_.load(std::memory_order_relaxed)) {
      continue;
    }

    ChunkBuffer pcm;
    if (!item.payload) {
      // Pool-exhausted placeholder upstream - codec-independent, checked
      // first so it can't be shadowed by the codec branches below.
      pcm = acquireChunkBuffer(
          reinterpret_cast<const std::byte*>(kSilenceChunk.data()),
          kSilenceChunk.size() * sizeof(int16_t));
    } else if (item.codec == Codec::Opus) {
      auto decoded = client_.decodeOpus(tcb::span<const std::byte>(
          item.payload.data(), item.payload.size()));
      if (!decoded) {
        pcm = acquireChunkBuffer(
            reinterpret_cast<const std::byte*>(kSilenceChunk.data()),
            kSilenceChunk.size() * sizeof(int16_t));
      } else {
        pcm = std::move(*decoded);
        if (pcm) {
          samplesPerChunkHint_.store(
              static_cast<uint32_t>(pcm.size() / kBytesPerFrame),
              std::memory_order_relaxed);
        }
      }
    } else if (item.codec == Codec::Flac) {
      auto decoded = client_.decodeFlac(tcb::span<const std::byte>(
          item.payload.data(), item.payload.size()));
      if (!decoded) {
        pcm = acquireChunkBuffer(
            reinterpret_cast<const std::byte*>(kSilenceChunk.data()),
            kSilenceChunk.size() * sizeof(int16_t));
      } else {
        pcm = std::move(*decoded);
        if (pcm) {
          samplesPerChunkHint_.store(
              static_cast<uint32_t>(pcm.size() / kBytesPerFrame),
              std::memory_order_relaxed);
        }
      }
    } else {
      pcm = std::move(item.payload);
      samplesPerChunkHint_.store(
          static_cast<uint32_t>(pcm.size() / kBytesPerFrame),
          std::memory_order_relaxed);
    }

    if (!pcm) {
      droppedChunkFrames_.fetch_add(
          samplesPerChunkHint_.load(std::memory_order_relaxed),
          std::memory_order_relaxed);
      const int64_t now = nowUs();
      if (now - lastAllocDropYieldUs >= kAllocDropYieldMs * 1000) {
        lastAllocDropYieldUs = now;
        std::this_thread::sleep_for(std::chrono::milliseconds(kAllocDropYieldMs));
      }
      continue;
    }

    // Codec may have changed mid-decode - recheck before publishing.
    if (item.codecGen != codecGeneration_.load(std::memory_order_relaxed)) {
      continue;
    }

    const auto sampleRate = static_cast<bell::audio::SampleRate>(
        sampleRateHz_.load(std::memory_order_relaxed));
    dsp_.process(pcm.data(), pcm.size(), pcm.data(), pcm.size(), sampleRate);

    pcmQueue_.push(DecodedChunk{item.serverTimeUs, std::move(pcm)});
  }
}

}  // namespace snapclient
