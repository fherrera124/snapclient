#include "snapclient/DecoderTask.h"

#include <bell/audio/SampleConversion.h>
#include <tcb/span.hpp>

#include "snapclient/SnapcastClient.h"

namespace snapclient {

namespace {

ChunkBuffer store(tcb::span<const std::byte> pcm) {
  return acquirePooledChunkBuffer(pcm.data(), pcm.size());
}

}  // namespace

DecoderTask::DecoderTask(BoundedQueue<QueuedChunk>& rawQueue,
                         BoundedQueue<DecodedChunk>& pcmQueue,
                         std::atomic<uint32_t>& codecGeneration,
                         SnapcastClient& client,
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
      samplesPerChunkHint_(samplesPerChunkHint) {
  startTask();
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

    ChunkBuffer pcm;
    if (item.payload) {
      const tcb::span<const std::byte> encoded(item.payload.data(),
                                               item.payload.size());
      if (item.codec == Codec::Opus) {
        if (auto decoded = client_.decodeOpus(encoded)) {
          pcm = store(decoded->pcm());
        }
      } else if (item.codec == Codec::Flac) {
        if (auto decoded = client_.decodeFlac(encoded)) {
          pcm = store(decoded->pcm());
        }
      } else if (const PcmLayout layout = client_.pcmLayout();
                 !layout.isS16()) {
        const size_t len = bell::audio::narrowToS16(
            {item.payload.data(), item.payload.size()}, layout.bits,
            layout.containerBytes);
        pcm = store({item.payload.data(), len});
      } else {
        pcm = std::move(item.payload);
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
