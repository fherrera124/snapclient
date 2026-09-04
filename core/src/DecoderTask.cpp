#include "snapclient/DecoderTask.h"

#include <bell/Logger.h>
#include <tcb/span.hpp>

#include "snapclient/SnapcastClient.h"

namespace snapclient {


DecoderTask::DecoderTask(BoundedQueue<QueuedChunk>& rawQueue,
                         BoundedQueue<DecodedChunk>& pcmQueue,
                         std::atomic<uint32_t>& codecGeneration,
                         SnapcastClient& client, DspProcessor& dsp,
                         std::atomic<uint32_t>& sampleRateHz,
                         std::atomic<uint32_t>& samplesPerChunkHint)
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
      samplesPerChunkHint_(samplesPerChunkHint) {
  startTask();
}

// dsp works on int16 samples, so it has to run on the codec's own output:
// a pool slot may only tolerate aligned 32-bit access.
ChunkBuffer DecoderTask::processAndStore(tcb::span<std::byte> pcm,
                                         bell::audio::SampleRate sampleRate) {
  dsp_.process(pcm.data(), pcm.size(), pcm.data(), pcm.size(), sampleRate);
  return acquirePooledChunkBuffer(pcm.data(), pcm.size());
}

void DecoderTask::runTask() {
  while (true) {
    QueuedChunk item;
    if (!rawQueue_.tryPop(item, 10)) {
      continue;
    }

    if (item.codecGen != codecGeneration_.load(std::memory_order_relaxed)) {
      continue;
    }

    const auto sampleRate = static_cast<bell::audio::SampleRate>(
        sampleRateHz_.load(std::memory_order_relaxed));

    ChunkBuffer pcm;
    if (item.payload) {
      const tcb::span<const std::byte> encoded(item.payload.data(),
                                               item.payload.size());
      if (item.codec == Codec::Opus) {
        if (auto decoded = client_.decodeOpus(encoded)) {
          pcm = processAndStore(decoded->pcm(), sampleRate);
        }
      } else if (item.codec == Codec::Flac) {
        if (auto decoded = client_.decodeFlac(encoded)) {
          pcm = processAndStore(decoded->pcm(), sampleRate);
        }
      } else {
        // Byte-addressable heap, so dsp can work in place here.
        pcm = std::move(item.payload);
        dsp_.process(pcm.data(), pcm.size(), pcm.data(), pcm.size(),
                     sampleRate);
      }
      if (pcm) {
        samplesPerChunkHint_.store(
            static_cast<uint32_t>(pcm.size() / kBytesPerFrame),
            std::memory_order_relaxed);
      }
    }

    // Codec may have changed mid-decode - recheck before publishing.
    if (item.codecGen != codecGeneration_.load(std::memory_order_relaxed)) {
      continue;
    }

    if (!pcm) {
      // Every chunk that got this far still has to produce its full
      // duration of output.
      pcmQueue_.push(DecodedChunk{
          item.serverTimeUs, ChunkBuffer(),
          samplesPerChunkHint_.load(std::memory_order_relaxed)});
      continue;
    }

    pcmQueue_.push(DecodedChunk{item.serverTimeUs, std::move(pcm), 0});
  }
}

}  // namespace snapclient
