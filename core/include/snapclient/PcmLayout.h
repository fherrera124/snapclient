#pragma once

#include <cstdint>
#include <optional>

#include <bell/audio/SampleConversion.h>

namespace snapclient {

// How a Pcm stream stores each sample: `bits` of it, in `containerBytes`.
struct PcmLayout {
  uint8_t bits = 16;
  uint8_t containerBytes = 2;

  bool isS16() const { return bits == 16 && containerBytes == 2; }
};

// From the codec header's WAV fields. nullopt for anything that cannot be
// narrowed to S16.
inline std::optional<PcmLayout> pcmLayoutFor(uint16_t bits, uint16_t channels,
                                             uint16_t blockAlign) {
  if (channels == 0 || blockAlign % channels != 0) {
    return std::nullopt;
  }
  const uint32_t containerBytes = blockAlign / channels;
  if (!bell::audio::canNarrowToS16(bits, containerBytes)) {
    return std::nullopt;
  }
  return PcmLayout{static_cast<uint8_t>(bits),
                   static_cast<uint8_t>(containerBytes)};
}

}  // namespace snapclient
