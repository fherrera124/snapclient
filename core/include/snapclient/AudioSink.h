#pragma once

#include <cstddef>
#include <cstdint>

namespace snapclient {

// What SyncEngine's caller (PlaybackPipeline) needs from an audio output -
// deliberately only what consumeOnce() actually calls, so a test double
// can implement it trivially.
class AudioSink {
 public:
  virtual ~AudioSink() = default;
  virtual void configure(uint32_t sampleRate) = 0;
  virtual void write(const std::byte* pcm, size_t len) = 0;
  virtual void setMuted(bool muted) = 0;
  virtual uint32_t sendQueueOverflowCount() const = 0;
  virtual uint32_t underrunCompensationFrames() const = 0;
};

}  // namespace snapclient
