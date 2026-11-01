#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <bell/audio/Common.h>
#include <bell/dsp/BiquadParameters.h>
#include <bell/dsp/Engine.h>
#include <bell/dsp/GainTransform.h>

namespace snapclient {

enum class DspFlow { Stereo, Biamp, BassBoost, EQBassTreble };

struct DspFilterParams {
  float freqPrimaryHz = 0.0f;
  float gainPrimaryDb = 0.0f;
  float freqTertiaryHz = 0.0f;
  float gainTertiaryDb = 0.0f;
};

// Highest gain the cascade reaches between 20 Hz and 20 kHz, in dB; 0 when
// it never rises above unity.
float biquadPeakGainDb(std::vector<bell::dsp::BiquadParameters> stages);

struct DspStages {
  std::vector<bell::dsp::BiquadParameters> left;
  std::vector<bell::dsp::BiquadParameters> right;
  // Both channels get the same stages.
  bool linked = true;
};

DspStages dspFlowStages(DspFlow flow, const DspFilterParams& params);

// Two-channel interleaved PCM, S16 in and S32 out.
class DspProcessor {
 public:
  DspProcessor();

  void switchFlow(DspFlow flow);
  void setDownmixMono(bool downmix);
  void setParams(DspFlow flow, const DspFilterParams& params);
  void setVolume(float volume);

  // out is full scale at INT32_MAX. Throws std::bad_alloc when the engine
  // cannot size its buffers.
  void process(const int16_t* in, int32_t* out, size_t frames,
               bell::audio::SampleRate sampleRate);

 private:
  void rebuildPipeline();

  bell::dsp::Engine engine;
  DspFlow activeFlow = DspFlow::Stereo;
  std::array<DspFilterParams, 4> flowParams{};
  float volume = 1.0f;
  bool downmixMono = false;
  // Owned across rebuilds so setVolume() can reconfigure it in place
  // without rebuilding the pipeline.
  std::shared_ptr<bell::dsp::GainTransform> gainTransform;
};

}  // namespace snapclient
