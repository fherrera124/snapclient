#include "snapclient/DecoderTask.h"

#include <chrono>
#include <thread>
#include <vector>

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

const std::vector<int16_t> kSilenceChunk(kFramesPerChunk * 2, 0);
}  // namespace

DecoderTask::DecoderTask(BoundedQueue<QueuedChunk>& rawQueue,
                         BoundedQueue<DecodedChunk>& pcmQueue,
                         std::atomic<uint32_t>& codecGeneration,
                         SnapcastClient& client)
    : bell::Task("decoder", 16 * 1024, /*espPriority=*/4,
                bell::TaskCore::CoreAny, /*espStackOnPsram=*/false),
      rawQueue_(rawQueue),
      pcmQueue_(pcmQueue),
      codecGeneration_(codecGeneration),
      client_(client) {
  startTask();
}

void DecoderTask::runTask() {
  // Heap, not stack - this task's stack is an unmeasured, conservative
  // guess, and a buffer this size is enough to overflow a tight one.
  std::vector<std::byte> decodeBuf(kPcmChunkBytes);
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
      auto decoded = client_.decodeOpus(
          tcb::span<const std::byte>(item.payload.data(),
                                     item.payload.size()),
          decodeBuf.data(), decodeBuf.size());
      if (!decoded) {
        pcm = acquireChunkBuffer(
            reinterpret_cast<const std::byte*>(kSilenceChunk.data()),
            kSilenceChunk.size() * sizeof(int16_t));
      } else {
        pcm = acquireChunkBuffer(decodeBuf.data(), *decoded);
      }
    } else {
      pcm = std::move(item.payload);
    }

    if (!pcm) {
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

    pcmQueue_.push(DecodedChunk{item.serverTimeUs, std::move(pcm)});
  }
}

}  // namespace snapclient
