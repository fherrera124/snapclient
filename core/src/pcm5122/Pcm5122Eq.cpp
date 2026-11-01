#include "snapclient/pcm5122/Pcm5122Eq.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "snapclient/DspProcessor.h"

namespace snapclient {

namespace {

constexpr double kIq28 = 1 << 28;
constexpr double kQ23 = 1 << 23;
constexpr double kQ22 = 1 << 22;
constexpr int32_t kWordMax = (1 << 23) - 1;
constexpr int32_t kWordMin = -(1 << 23);

// value in a 24-bit word where one is 1.0; nullopt if it does not fit.
std::optional<uint32_t> toWord(double value, double one) {
  const double scaled = std::round(value * one);
  if (!std::isfinite(scaled) || scaled > kWordMax || scaled < kWordMin) {
    return std::nullopt;
  }
  return static_cast<uint32_t>(static_cast<int32_t>(scaled)) & 0xFFFFFF;
}

}  // namespace

std::optional<Pcm5122Eq> pcm5122Eq(
    const std::vector<bell::dsp::BiquadParameters>& stages,
    uint32_t sampleRate) {
  if (stages.size() > Pcm5122Eq::kStages) {
    return std::nullopt;
  }
  Pcm5122Eq eq;
  for (size_t i = 0; i < Pcm5122Eq::kStages; i++) {
    eq.biquads[i * 5] = kWordMax;
  }

  // Gain taken out of the stages so far, and the most the chain may pass.
  double scale = 1.0;
  double headroom = 1.0;
  try {
    for (size_t i = 0; i < stages.size(); i++) {
      auto stage = stages[i];
      // {a1, a2, b0, b1, b2}, with a1 and a2 already negated.
      const auto c = stage.calculateCoefficients(
          static_cast<bell::audio::SampleRate>(sampleRate));
      const double a1 = c[0] / kIq28;
      const double a2 = c[1] / kIq28;
      const double b0 = c[2] / kIq28;
      const double b1 = c[3] / kIq28;
      const double b2 = c[4] / kIq28;

      const std::vector<bell::dsp::BiquadParameters> prefix(
          stages.begin(), stages.begin() + i + 1);
      headroom = std::min(
          headroom, std::pow(10.0, -biquadPeakGainDb(prefix) / 20.0));
      // The chip doubles N1 and D1.
      const double fit = kWordMax / kQ23 /
                         std::max({std::abs(b0), std::abs(b1) / 2,
                                   std::abs(b2)});
      const double gain = std::min({1.0, fit, headroom / scale});
      scale *= gain;

      const double words[5] = {b0 * gain, b1 * gain / 2, b2 * gain, a1 / 2,
                               a2};
      for (size_t j = 0; j < 5; j++) {
        const auto word = toWord(words[j], kQ23);
        if (!word) {
          return std::nullopt;
        }
        eq.biquads[i * 5 + j] = *word;
      }
    }
  } catch (const std::invalid_argument&) {
    return std::nullopt;
  }

  const auto volume = toWord(headroom / scale, kQ22);
  if (!volume) {
    return std::nullopt;
  }
  eq.volume = *volume;
  return eq;
}

// From sonocotta/esphome-pcm5122 (GPLv3), with the DRC curve (C88-C94)
// flat, the mixer (C148-C150) passing its first input alone, and the
// volume (C158) at 1.0.
const std::array<uint32_t, 142> kPcm5122Program5Image = {
    0x7FFFFF, 0x000000, 0x000000, 0x000000, 0x000000,  // C40
    0x7FFFFF, 0x000000, 0x000000, 0x000000, 0x000000,  // C45
    0x7FFFFF, 0x000000, 0x000000, 0x000000, 0x000000,  // C50
    0x7FFFFF, 0x000000, 0x000000, 0x000000, 0x000000,  // C55
    0x7FFFFF, 0x000000, 0x000000, 0x000000, 0x000000,  // C60
    0x7FFFFF, 0x000000, 0x000000, 0x000000, 0x000000,  // C65
    0x0000AF, 0x7FFF51, 0x0002BB, 0x7FFD45, 0x000057,  // C70
    0x7FFFA9, 0x0006D3, 0x7FF92D, 0x001B4C, 0x7FE4B4,  // C75
    0x00036A, 0x7FFC96, 0x004432, 0x7FBBCE, 0x010FEF,  // C80
    0x7EF011, 0x00221E, 0x7FDDE2, 0x000000, 0x000000,  // C85
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000,  // C90
    0xC00000, 0x800000, 0x400000, 0x000000, 0x000000,  // C95
    0xD741A0, 0xFFF000, 0x880000, 0x180000, 0x000000,  // C100
    0x000000, 0x000000, 0x300000, 0xD00000, 0x000384,  // C105
    0x00B255, 0x0E5BDE, 0x7FFFFF, 0x000000, 0x020000,  // C110
    0x200000, 0x005126, 0x400000, 0x000CDC, 0x020000,  // C115
    0x000000, 0x000000, 0x00165A, 0x003756, 0x10A9BF,  // C120
    0x7FFFFF, 0x000000, 0x020000, 0xE00000, 0x0C9E80,  // C125
    0xC00000, 0x4F9F16, 0x020000, 0x000000, 0x000000,  // C130
    0x200000, 0x400000, 0xE00000, 0xC00000, 0x000000,  // C135
    0x000000, 0x000000, 0x800000, 0x7FFFFF, 0x000000,  // C140
    0x000000, 0x000000, 0xC00000, 0x7FFFFF, 0x000000,  // C145
    0x000000, 0x2E0000, 0x000001, 0x000000, 0x400000,  // C150
    0x400000, 0x418000, 0x000000, 0x400000, 0x000000,  // C155
    0x000000, 0xC00000, 0x000000, 0x000000, 0x000000,  // C160
    0x000000, 0x000000, 0x000000, 0x400000, 0x000000,  // C165
    0xFFFFFF, 0x800000, 0x400000, 0x7FFFFF, 0x000000,  // C170
    0x1A0000, 0x118000, 0x280000, 0x278000, 0x7FFFFF,  // C175
    0x2E0000, 0x418000,  // C180
};

}  // namespace snapclient
