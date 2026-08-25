#pragma once

#include <array>
#include <cstddef>

#include <bell/audio/Common.h>
#include <bell/dsp/Engine.h>

namespace snapclient {

enum class DspFlow { Stereo, Biamp, BassBoost, EQBassTreble };

struct DspFilterParams {
  float freqPrimaryHz = 0.0f;
  float gainPrimaryDb = 0.0f;
  float freqTertiaryHz = 0.0f;
  float gainTertiaryDb = 0.0f;
};

// Two-channel S16 interleaved PCM, same format in and out.
class DspProcessor {
 public:
  DspProcessor();

  void switchFlow(DspFlow flow);
  void setParams(DspFlow flow, const DspFilterParams& params);
  void setVolume(float volume);

  void process(const std::byte* in, size_t inLen, std::byte* out,
               size_t outLen, bell::audio::SampleRate sampleRate);

 private:
  void rebuildPipeline();

  bell::dsp::Engine engine;
  DspFlow activeFlow = DspFlow::Stereo;
  std::array<DspFilterParams, 4> flowParams{};
  float volume = 1.0f;
};

}  // namespace snapclient
