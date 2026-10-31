#include "snapclient/DspProcessor.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <memory>

#include <bell/dsp/BiquadTransform.h>
#include <bell/dsp/GainTransform.h>
#include <bell/dsp/MixerTransform.h>
#include <bell/dsp/TransformPipeline.h>

namespace snapclient {

using bell::dsp::BiquadParameters;
using bell::dsp::BiquadTransform;
using bell::dsp::GainTransform;
using bell::dsp::MixerTransform;
using bell::dsp::TransformPipeline;

namespace {
constexpr float kShelfQ = 0.707f;

// A BiquadTransform filters only the first channel it is given.
std::shared_ptr<BiquadTransform> makeBiquad(
    int channel, const std::vector<BiquadParameters>& stages) {
  auto transform = std::make_shared<BiquadTransform>();
  transform->setChannels({channel});
  for (const auto& stage : stages) {
    transform->addStage(stage);
  }
  return transform;
}
}  // namespace

float biquadPeakGainDb(std::vector<BiquadParameters> stages) {
  constexpr auto kRate = bell::audio::SampleRate::SR_48000HZ;
  constexpr float kRateHz = 48000.0f;
  constexpr float kIq28 = 1 << 28;
  constexpr float kLowHz = 20.0f;
  constexpr float kHighHz = 20000.0f;
  constexpr int kPoints = 256;

  // {a1, a2, b0, b1, b2}, with a1 and a2 already negated.
  std::vector<std::array<float, 5>> coeffs;
  for (auto& stage : stages) {
    const auto c = stage.calculateCoefficients(kRate);
    coeffs.push_back({c[0] / kIq28, c[1] / kIq28, c[2] / kIq28,
                      c[3] / kIq28, c[4] / kIq28});
  }
  if (coeffs.empty()) {
    return 0.0f;
  }

  const float step = std::pow(kHighHz / kLowHz, 1.0f / (kPoints - 1));
  float peak = 1.0f;
  float hz = kLowHz;
  for (int i = 0; i < kPoints; i++, hz *= step) {
    const auto z1 = std::polar(1.0f, -2.0f * static_cast<float>(M_PI) * hz /
                                         kRateHz);
    const auto z2 = z1 * z1;
    std::complex<float> h = 1.0f;
    for (const auto& c : coeffs) {
      h *= (c[2] + c[3] * z1 + c[4] * z2) / (1.0f - c[0] * z1 - c[1] * z2);
    }
    peak = std::max(peak, std::abs(h));
  }
  return 20.0f * std::log10(peak);
}

DspProcessor::DspProcessor() {
  rebuildPipeline();
}

void DspProcessor::switchFlow(DspFlow flow) {
  activeFlow = flow;
  rebuildPipeline();
}

void DspProcessor::setDownmixMono(bool downmix) {
  downmixMono = downmix;
  rebuildPipeline();
}

void DspProcessor::setParams(DspFlow flow, const DspFilterParams& params) {
  flowParams[static_cast<size_t>(flow)] = params;
  if (flow == activeFlow) {
    rebuildPipeline();
  }
}

void DspProcessor::setVolume(float volume) {
  this->volume = volume;
  // GainTransform::configure() is thread-safe on its own - no need to
  // touch the pipeline.
  if (gainTransform) {
    gainTransform->configure(20.0f * log10f(volume));
  }
}

void DspProcessor::rebuildPipeline() {
  const auto& params = flowParams[static_cast<size_t>(activeFlow)];
  std::vector<BiquadParameters> left;
  std::vector<BiquadParameters> right;

  switch (activeFlow) {
    case DspFlow::Stereo:
      break;

    case DspFlow::EQBassTreble:
      left = {BiquadParameters(BiquadParameters::Type::Lowshelf,
                               params.freqPrimaryHz, kShelfQ,
                               params.gainPrimaryDb, std::nullopt,
                               std::nullopt),
              BiquadParameters(BiquadParameters::Type::Highshelf,
                               params.freqTertiaryHz, kShelfQ,
                               params.gainTertiaryDb, std::nullopt,
                               std::nullopt)};
      right = left;
      break;

    case DspFlow::BassBoost:
      left = {BiquadParameters(BiquadParameters::Type::Lowshelf,
                               params.freqPrimaryHz, kShelfQ,
                               params.gainPrimaryDb, std::nullopt,
                               std::nullopt)};
      right = left;
      break;

    case DspFlow::Biamp:
      left = {BiquadParameters(BiquadParameters::Type::Lowpass,
                               params.freqPrimaryHz, kShelfQ, std::nullopt,
                               std::nullopt, std::nullopt)};
      right = {BiquadParameters(BiquadParameters::Type::Highpass,
                                params.freqTertiaryHz, kShelfQ, std::nullopt,
                                std::nullopt, std::nullopt)};
      break;
  }

  auto pipeline = std::make_shared<TransformPipeline>();
  // Attenuate by the boost before it is applied, so it cannot clip.
  const float headroomDb =
      std::max(biquadPeakGainDb(left), biquadPeakGainDb(right));
  if (headroomDb > 0.0f) {
    auto headroom = std::make_shared<GainTransform>();
    headroom->setChannels({0, 1});
    headroom->configure(-headroomDb);
    pipeline->addTransform(headroom);
  }
  if (!left.empty()) {
    pipeline->addTransform(makeBiquad(0, left));
  }
  if (!right.empty()) {
    pipeline->addTransform(makeBiquad(1, right));
  }

  gainTransform = std::make_shared<GainTransform>();
  gainTransform->setChannels({0, 1});
  gainTransform->configure(20.0f * log10f(volume));
  pipeline->addTransform(gainTransform);

  if (downmixMono) {
    // Both outputs get the mix, so a mono amp is fed whichever it takes.
    auto mixer = std::make_shared<MixerTransform>();
    mixer->configure({{0, 1}, {0, 1}});
    pipeline->addTransform(mixer);
  }

  engine.applyPipeline(pipeline);
}

void DspProcessor::process(const std::byte* in, size_t inLen, std::byte* out,
                           size_t outLen, bell::audio::SampleRate sampleRate) {
  bell::audio::Format format(2, bell::audio::SampleFormat::S16, sampleRate);
  engine.process(in, inLen, out, outLen, format);
}

}  // namespace snapclient
