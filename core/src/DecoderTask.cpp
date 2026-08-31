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
const char* kLogTag = "decoder_task";

constexpr int64_t kTimingLogIntervalUs = 10'000'000;   // 10s
constexpr int64_t kWarnLogIntervalUs = 30'000'000;     // 30s
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
  // guess (see stackHighWaterMarkWords()), and a buffer this size is
  // enough to overflow a tight one.
  std::vector<std::byte> decodeBuf(kPcmChunkBytes);

  int64_t decodeSumUs = 0, decodeMaxUs = 0;
  size_t decodeSamples = 0;
  size_t silencePlayed = 0, staleDropped = 0, allocDropped = 0;
  int64_t lastTimingLogUs = nowUs();
  int64_t lastWarnLogUs = nowUs();
  int64_t lastAllocDropYieldUs = 0;

  while (true) {
    const int64_t logNow = nowUs();
    if (logNow - lastTimingLogUs >= kTimingLogIntervalUs) {
      lastTimingLogUs = logNow;
      BELL_LOG(info, kLogTag, "loop timing: decodeAvg={} decodeMax={} n={}",
               decodeSamples ? decodeSumUs / decodeSamples : 0, decodeMaxUs,
               decodeSamples);
      decodeSumUs = 0;
      decodeMaxUs = 0;
      decodeSamples = 0;
    }
    if (logNow - lastWarnLogUs >= kWarnLogIntervalUs) {
      lastWarnLogUs = logNow;
      if (silencePlayed > 0 || staleDropped > 0 || allocDropped > 0) {
        BELL_LOG(warn, kLogTag,
                 "silencePlayed={} staleDropped={} allocDropped={} in the "
                 "last 30s",
                 silencePlayed, staleDropped, allocDropped);
        silencePlayed = 0;
        staleDropped = 0;
        allocDropped = 0;
      }
    }

    QueuedChunk item;
    if (!rawQueue_.tryPop(item, 10)) {
      continue;
    }

    if (item.codecGen != codecGeneration_.load(std::memory_order_relaxed)) {
      staleDropped++;
      continue;
    }

    ChunkBuffer pcm;
    if (!item.payload) {
      // Pool-exhausted placeholder upstream - codec-independent, checked
      // first so it can't be shadowed by the codec branches below.
      pcm = acquireChunkBuffer(
          reinterpret_cast<const std::byte*>(kSilenceChunk.data()),
          kSilenceChunk.size() * sizeof(int16_t));
      silencePlayed++;
    } else if (item.codec == Codec::Opus) {
      const int64_t decodeStartUs = nowUs();
      auto decoded = client_.decodeOpus(
          tcb::span<const std::byte>(item.payload.data(),
                                     item.payload.size()),
          decodeBuf.data(), decodeBuf.size());
      const int64_t decodeUs = nowUs() - decodeStartUs;
      decodeSumUs += decodeUs;
      decodeMaxUs = std::max(decodeMaxUs, decodeUs);
      decodeSamples++;
      if (!decoded) {
        pcm = acquireChunkBuffer(
            reinterpret_cast<const std::byte*>(kSilenceChunk.data()),
            kSilenceChunk.size() * sizeof(int16_t));
        silencePlayed++;
      } else {
        pcm = acquireChunkBuffer(decodeBuf.data(), *decoded);
      }
    } else {
      pcm = std::move(item.payload);
    }

    if (!pcm) {
      allocDropped++;
      const int64_t now = nowUs();
      if (now - lastAllocDropYieldUs >= kAllocDropYieldMs * 1000) {
        lastAllocDropYieldUs = now;
        std::this_thread::sleep_for(std::chrono::milliseconds(kAllocDropYieldMs));
      }
      continue;
    }

    // Codec may have changed mid-decode - recheck before publishing.
    if (item.codecGen != codecGeneration_.load(std::memory_order_relaxed)) {
      staleDropped++;
      continue;
    }

    pcmQueue_.push(DecodedChunk{item.serverTimeUs, std::move(pcm)});
  }
}

}  // namespace snapclient
