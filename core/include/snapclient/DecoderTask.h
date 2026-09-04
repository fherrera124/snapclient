#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <bell/audio/Common.h>
#include <bell/utils/Task.h>
#include <tcb/span.hpp>

#include "snapclient/BoundedQueue.h"
#include "snapclient/ChunkBuffer.h"
#include "snapclient/DspProcessor.h"
#include "snapclient/Protocol.h"

namespace snapclient {

class SnapcastClient;

constexpr size_t kBytesPerFrame = 2 * sizeof(int16_t);  // stereo S16

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
  // Frames to emit as zeros when pcm carries no buffer - a chunk that
  // can't be decoded still has to occupy its slot in the timeline.
  size_t silentFrames = 0;

  size_t frames() const {
    return pcm ? pcm.size() / kBytesPerFrame : silentFrames;
  }
  // nullptr for a silent placeholder - the output path zero-fills instead.
  const std::byte* dataAt(size_t offsetFrames) const {
    return pcm ? pcm.data() + offsetFrames * kBytesPerFrame : nullptr;
  }
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
  ChunkBuffer processAndStore(tcb::span<std::byte> pcm,
                              bell::audio::SampleRate sampleRate);

  BoundedQueue<QueuedChunk>& rawQueue_;
  BoundedQueue<DecodedChunk>& pcmQueue_;
  std::atomic<uint32_t>& codecGeneration_;
  SnapcastClient& client_;
  DspProcessor& dsp_;
  std::atomic<uint32_t>& sampleRateHz_;
  // Real samples-per-chunk of the last chunk decoded, written every chunk.
  std::atomic<uint32_t>& samplesPerChunkHint_;
};

}  // namespace snapclient
