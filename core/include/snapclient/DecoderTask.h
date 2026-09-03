#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <bell/utils/Task.h>

#include "snapclient/BoundedQueue.h"
#include "snapclient/ChunkBuffer.h"
#include "snapclient/DspProcessor.h"
#include "snapclient/Protocol.h"

namespace snapclient {

class SnapcastClient;

constexpr size_t kBytesPerFrame = 2 * sizeof(int16_t);  // stereo S16
// decodeBuf's fixed size - must cover the largest single decode() call's
// output across every codec, allocated once before any chunk exists so it
// can't be sized from an observed value. 4096 samples/channel covers
// libFLAC's own block size at snapserver's default (and higher) FLAC
// compression levels; Opus's fixed 960-sample chunks fit comfortably
// within it too.
constexpr size_t kMaxDecodedChunkBytes = 4096 * kBytesPerFrame;

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

// Pulls raw chunks off rawQueue, decodes (or substitutes silence), applies
// dsp, and pushes ready PCM onto pcmQueue - runs on its own task so decode
// overlaps the consumer's output work on the previous chunk instead of
// running inline before it. dsp runs here too (not in the consumer) so
// pcmQueue already holds fully-processed audio - the consumer's own job is
// then just sync timing and resampling, both of which do depend on
// per-call state dsp doesn't need.
class DecoderTask : public bell::Task {
 public:
  DecoderTask(BoundedQueue<QueuedChunk>& rawQueue,
              BoundedQueue<DecodedChunk>& pcmQueue,
              std::atomic<uint32_t>& codecGeneration, SnapcastClient& client,
              DspProcessor& dsp, std::atomic<uint32_t>& sampleRateHz,
              std::atomic<uint32_t>& samplesPerChunkHint);

 protected:
  void runTask() override;

 private:
  BoundedQueue<QueuedChunk>& rawQueue_;
  BoundedQueue<DecodedChunk>& pcmQueue_;
  std::atomic<uint32_t>& codecGeneration_;
  SnapcastClient& client_;
  DspProcessor& dsp_;
  std::atomic<uint32_t>& sampleRateHz_;
  // Most recent real (decoded, or passthrough for Pcm) samples-per-chunk
  // count - written here after every chunk, read by
  // PlaybackPipeline::applyQueueCapacity() instead of a fixed per-codec
  // constant.
  std::atomic<uint32_t>& samplesPerChunkHint_;
};

}  // namespace snapclient
