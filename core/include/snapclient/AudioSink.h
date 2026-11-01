#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <bell/dsp/BiquadParameters.h>

namespace snapclient {

// write() and preload() take interleaved stereo S32, full scale at INT32_MAX.
constexpr size_t kSinkBytesPerFrame = 2 * sizeof(int32_t);

// What SyncEngine's caller (PlaybackPipeline) needs from an audio output -
// deliberately only what consumeOnce() actually calls, so a test double
// can implement it trivially.
class AudioSink {
 public:
  virtual ~AudioSink() = default;
  // chunkFrames is the codec's real samples-per-chunk. A sink pacing output
  // through fixed-size hardware buffers must size those to divide it evenly;
  // a partial remainder mis-tracks real elapsed time.
  virtual void configure(uint32_t sampleRate, uint32_t chunkFrames) = 0;
  virtual void write(const std::byte* pcm, size_t len) = 0;
  virtual void setMuted(bool muted) = 0;
  // Linear gain in [0, 1]. True when the output applies it itself, so the
  // samples must reach it at full scale.
  virtual bool setVolume(float /*gain*/) { return false; }
  // Filters both channels with stages. True when the output applies them
  // itself, so the samples must reach it unfiltered; false leaves it flat.
  virtual bool setEq(
      const std::vector<bell::dsp::BiquadParameters>& /*stages*/) {
    return false;
  }

  // Non-blocking, best-effort: pushes as much of pcm[0..len) into the
  // output buffer as currently fits, returning bytes actually accepted
  // (may be less than len). Only valid while disabled.
  virtual size_t preload(const std::byte* pcm, size_t len) = 0;
  // Stops output so preload() can fill it ahead of time; enable() resumes
  // with whatever was preloaded flowing immediately.
  virtual void disable() = 0;
  virtual void enable() = 0;

  // Frames the output path can hold buffered ahead of playback (e.g.
  // dma_desc_num * dma_frame_num). 0 if not configured.
  virtual uint32_t ringCapacityFrames() const = 0;

  // How much later than its nominal sample rate the output has played
  // since the last enable(), in local microseconds. Non-zero only for an
  // output clocked by something other than the local timebase.
  virtual int64_t clockDriftUs() const { return 0; }
};

}  // namespace snapclient
