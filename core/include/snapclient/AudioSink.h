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

  // Non-blocking, best-effort: pushes as much of pcm[0..len) into the
  // output buffer as currently fits, returning bytes actually accepted
  // (may be less than len). Only valid while disabled.
  virtual size_t preload(const std::byte* pcm, size_t len) = 0;
  // Stops output so preload() can fill it ahead of time; enable() resumes
  // with whatever was preloaded flowing immediately.
  virtual void disable() = 0;
  virtual void enable() = 0;
};

}  // namespace snapclient
