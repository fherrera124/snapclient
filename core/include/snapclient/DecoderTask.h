#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <bell/utils/Task.h>

#include "snapclient/BoundedQueue.h"
#include "snapclient/ChunkBuffer.h"
#include "snapclient/Protocol.h"

namespace snapclient {

class SnapcastClient;

// Snapcast's Opus stream here always encodes 20ms frames at 48kHz - 960
// samples per channel.
constexpr size_t kFramesPerChunk = 960;
constexpr size_t kBytesPerFrame = 2 * sizeof(int16_t);  // stereo S16
constexpr size_t kPcmChunkBytes = kFramesPerChunk * kBytesPerFrame;

struct QueuedChunk {
  int64_t serverTimeUs = 0;
  Codec codec = Codec::None;
  uint32_t codecGen = 0;
  // Encoded (Opus) or raw (Pcm codec) payload, exactly as received -
  // DecoderTask decodes it, not the caller pushing here.
  ChunkBuffer payload;
};

struct DecodedChunk {
  int64_t serverTimeUs = 0;
  ChunkBuffer pcm;
};

// Pulls raw chunks off rawQueue, decodes (or substitutes silence), and
// pushes ready PCM onto pcmQueue - runs on its own task so decode
// overlaps the consumer's dsp+output work on the previous chunk instead
// of running inline before it.
class DecoderTask : public bell::Task {
 public:
  DecoderTask(BoundedQueue<QueuedChunk>& rawQueue,
              BoundedQueue<DecodedChunk>& pcmQueue,
              std::atomic<uint32_t>& codecGeneration, SnapcastClient& client);

 protected:
  void runTask() override;

 private:
  BoundedQueue<QueuedChunk>& rawQueue_;
  BoundedQueue<DecodedChunk>& pcmQueue_;
  std::atomic<uint32_t>& codecGeneration_;
  SnapcastClient& client_;
};

}  // namespace snapclient
