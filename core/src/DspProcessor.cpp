#include "snapclient/DspProcessor.h"

#include <cmath>
#include <memory>

#include <bell/dsp/BiquadParameters.h>
#include <bell/dsp/BiquadTransform.h>
#include <bell/dsp/GainTransform.h>
#include <bell/dsp/TransformPipeline.h>

namespace snapclient {

using bell::dsp::BiquadParameters;
using bell::dsp::BiquadTransform;
using bell::dsp::GainTransform;
using bell::dsp::TransformPipeline;

namespace {
constexpr float kShelfQ = 0.707f;

std::shared_ptr<BiquadTransform> makeBiquad(
    const std::vector<int>& channels,
    std::initializer_list<BiquadParameters> stages) {
  auto transform = std::make_shared<BiquadTransform>();
  transform->setChannels(channels);
  for (const auto& stage : stages) {
    transform->addStage(stage);
  }
  return transform;
}
}  // namespace

DspProcessor::DspProcessor() {
  rebuildPipeline();
}

void DspProcessor::switchFlow(DspFlow flow) {
  activeFlow = flow;
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
  auto pipeline = std::make_shared<TransformPipeline>();

  switch (activeFlow) {
    case DspFlow::Stereo:
      break;

    case DspFlow::EQBassTreble:
      pipeline->addTransform(makeBiquad(
          {0, 1},
          {BiquadParameters(BiquadParameters::Type::Lowshelf,
                             params.freqPrimaryHz, kShelfQ,
                             params.gainPrimaryDb, std::nullopt, std::nullopt),
           BiquadParameters(BiquadParameters::Type::Highshelf,
                             params.freqTertiaryHz, kShelfQ,
                             params.gainTertiaryDb, std::nullopt,
                             std::nullopt)}));
      break;

    case DspFlow::BassBoost:
      pipeline->addTransform(makeBiquad(
          {0, 1}, {BiquadParameters(BiquadParameters::Type::Lowshelf,
                                    params.freqPrimaryHz, kShelfQ,
                                    params.gainPrimaryDb, std::nullopt,
                                    std::nullopt)}));
      break;

    case DspFlow::Biamp:
      pipeline->addTransform(makeBiquad(
          {0}, {BiquadParameters(BiquadParameters::Type::Lowpass,
                                 params.freqPrimaryHz, kShelfQ, std::nullopt,
                                 std::nullopt, std::nullopt)}));
      pipeline->addTransform(makeBiquad(
          {1}, {BiquadParameters(BiquadParameters::Type::Highpass,
                                 params.freqTertiaryHz, kShelfQ, std::nullopt,
                                 std::nullopt, std::nullopt)}));
      break;
  }

  gainTransform = std::make_shared<GainTransform>();
  gainTransform->setChannels({0, 1});
  gainTransform->configure(20.0f * log10f(volume));
  pipeline->addTransform(gainTransform);

  engine.applyPipeline(pipeline);
}

void DspProcessor::process(const std::byte* in, size_t inLen, std::byte* out,
                           size_t outLen, bell::audio::SampleRate sampleRate) {
  bell::audio::Format format(2, bell::audio::SampleFormat::S16, sampleRate);
  engine.process(in, inLen, out, outLen, format);
}

}  // namespace snapclient
