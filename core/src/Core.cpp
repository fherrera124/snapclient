#include "snapclient/Core.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

#include <bell/Logger.h>

#include "snapclient/DspProcessor.h"

namespace snapclient {

static const char* LOG_TAG = "snapclient";

void scaffoldSelfCheck() {
  BELL_LOG(info, LOG_TAG, "snapclient_core linked against bell, scaffold ok");
}

namespace {
constexpr float kPi = 3.14159265358979323846f;

int16_t peakOnChannel(const std::vector<int16_t>& interleaved, int channel) {
  int16_t peak = 0;
  for (size_t i = channel; i < interleaved.size(); i += 2) {
    peak = std::max(peak, static_cast<int16_t>(std::abs(interleaved[i])));
  }
  return peak;
}

struct FlowCase {
  DspFlow flow;
  const char* name;
  DspFilterParams params;
};
}  // namespace

bool dspSmokeTest() {
  constexpr size_t kFrames = 512;
  constexpr float kToneHz = 100.0f;
  constexpr float kSampleRate = 44100.0f;
  constexpr int16_t kAmplitude = 8000;

  std::vector<int16_t> input(kFrames * 2);
  for (size_t i = 0; i < kFrames; i++) {
    auto sample = static_cast<int16_t>(
        kAmplitude * sinf(2.0f * kPi * kToneHz * static_cast<float>(i) /
                          kSampleRate));
    input[i * 2] = sample;
    input[i * 2 + 1] = sample;
  }
  std::vector<int16_t> output(kFrames * 2);
  int16_t inPeak = peakOnChannel(input, 0);

  const FlowCase cases[] = {
      {DspFlow::Stereo, "Stereo", {}},
      {DspFlow::EQBassTreble, "EQBassTreble", {150.0f, 6.0f, 8000.0f, 3.0f}},
      {DspFlow::BassBoost, "BassBoost", {150.0f, 9.0f, 0.0f, 0.0f}},
      {DspFlow::Biamp, "Biamp", {500.0f, 0.0f, 500.0f, 0.0f}},
  };

  bool allOk = true;
  for (const auto& c : cases) {
    DspProcessor dsp;
    dsp.setParams(c.flow, c.params);
    dsp.switchFlow(c.flow);

    bool ok = true;
    try {
      dsp.process(reinterpret_cast<const std::byte*>(input.data()),
                  input.size() * sizeof(int16_t),
                  reinterpret_cast<std::byte*>(output.data()),
                  output.size() * sizeof(int16_t),
                  bell::audio::SampleRate::SR_44100HZ);
    } catch (const std::exception& e) {
      BELL_LOG(error, LOG_TAG, "{}: threw: {}", c.name, e.what());
      ok = false;
    }

    if (ok) {
      int16_t outPeak0 = peakOnChannel(output, 0);
      int16_t outPeak1 = peakOnChannel(output, 1);
      BELL_LOG(info, LOG_TAG, "{}: in={} out_ch0={} out_ch1={}", c.name,
               inPeak, outPeak0, outPeak1);

      switch (c.flow) {
        case DspFlow::Biamp:
          // ch0 (lowpass, 500Hz) should pass a 100Hz tone through mostly
          // unattenuated; ch1 (highpass, 500Hz) should attenuate it heavily.
          ok = outPeak1 < inPeak / 2;
          break;
        case DspFlow::BassBoost:
        case DspFlow::EQBassTreble:
          // A boosted low shelf should raise a 100Hz tone's peak.
          ok = outPeak0 > inPeak;
          break;
        case DspFlow::Stereo:
          break;
      }
    }

    BELL_LOG(info, LOG_TAG, "{}: {}", c.name, ok ? "PASS" : "FAIL");
    allOk = allOk && ok;
  }

  return allOk;
}

}  // namespace snapclient
