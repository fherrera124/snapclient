#pragma once
#include <cstdint>
#include <cstddef>

namespace snapclient {

class DynamicResampler {
 public:
  // Resamples 16-bit stereo PCM.
  // Converts 'inFrames' to 'outFrames' by smoothly compressing or stretching the waveform.
  static size_t process(const int16_t* in, size_t inFrames,
                        int16_t* out, size_t outFrames) {
    // Direct passthrough if SyncEngine set frameAdjustment = 0
    if (inFrames == outFrames) {
      for (size_t i = 0; i < inFrames * 2; ++i) {
        out[i] = in[i];
      }
      return outFrames;
    }

    // Fixed-point 16.16 format (16 integer bits, 16 fractional bits)
    // Replaces float multiplication/division with fast bitshifts.
    uint32_t step = (inFrames << 16) / outFrames;
    uint32_t phase = 0;

    for (size_t i = 0; i < outFrames; ++i) {
      uint32_t index = phase >> 16;
      uint32_t frac = phase & 0xFFFF; // Value between 0 and 65535

      // Prevent overflow on the last frame by interpolating with itself
      uint32_t nextIndex = (index + 1 < inFrames) ? index + 1 : index;

      // Interpolate left channel
      int32_t l0 = in[index * 2];
      int32_t l1 = in[nextIndex * 2];
      out[i * 2] = static_cast<int16_t>(l0 + (((l1 - l0) * static_cast<int32_t>(frac)) >> 16));

      // Interpolate right channel
      int32_t r0 = in[index * 2 + 1];
      int32_t r1 = in[nextIndex * 2 + 1];
      out[i * 2 + 1] = static_cast<int16_t>(r0 + (((r1 - r0) * static_cast<int32_t>(frac)) >> 16));

      phase += step;
    }
    
    return outFrames;
  }
};

}  // namespace snapclient
