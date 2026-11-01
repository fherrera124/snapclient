#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <bell/dsp/BiquadParameters.h>

namespace snapclient {

// Program 5 runs only up to this rate.
constexpr uint32_t kPcm5122Program5MaxRate = 48000;

// Program 5 coefficients for a stereo-linked EQ, as raw 24-bit words.
struct Pcm5122Eq {
  static constexpr size_t kStages = 6;
  static constexpr uint8_t kFirstBiquad = 10;
  static constexpr uint8_t kVolume = 158;

  std::array<uint32_t, kStages * 5> biquads{};
  uint32_t volume = 0;

  bool operator==(const Pcm5122Eq& other) const {
    return biquads == other.biquads && volume == other.volume;
  }
  bool operator!=(const Pcm5122Eq& other) const { return !(*this == other); }
};

// Every stage output stays within full scale and the chain peaks at 0 dB,
// like the software EQ's headroom. nullopt for more than six stages, a
// filter that will not fit the chip's coefficient range, or more than 6 dB
// of make-up gain.
std::optional<Pcm5122Eq> pcm5122Eq(
    const std::vector<bell::dsp::BiquadParameters>& stages,
    uint32_t sampleRate);

// The rest of Program 5's coefficient RAM, from C40.
constexpr uint8_t kPcm5122Program5ImageFirst = 40;
extern const std::array<uint32_t, 142> kPcm5122Program5Image;

}  // namespace snapclient
