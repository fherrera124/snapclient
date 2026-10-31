#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "snapclient/DspProcessor.h"

namespace {

int gFailures = 0;

void checkImpl(bool cond, const char* expr, const char* file, int line) {
  if (!cond) {
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
    gFailures++;
  }
}

}  // namespace

#define CHECK(cond) checkImpl((cond), #cond, __FILE__, __LINE__)

namespace snapclient {
namespace {

using bell::dsp::BiquadParameters;

constexpr float kRateHz = 44100.0f;
constexpr size_t kFrames = 8192;

std::vector<int16_t> tone(float hz, int16_t amplitude) {
  std::vector<int16_t> pcm(kFrames * 2);
  for (size_t i = 0; i < kFrames; i++) {
    const auto v = static_cast<int16_t>(std::lround(
        amplitude * std::sin(2.0 * M_PI * hz * static_cast<double>(i) /
                             kRateHz)));
    pcm[2 * i] = v;
    pcm[2 * i + 1] = v;
  }
  return pcm;
}

std::vector<int16_t> run(DspFlow flow, const DspFilterParams& params,
                         const std::vector<int16_t>& in) {
  DspProcessor dsp;
  dsp.setParams(flow, params);
  dsp.switchFlow(flow);
  std::vector<int16_t> out(in.size());
  dsp.process(reinterpret_cast<const std::byte*>(in.data()),
              in.size() * sizeof(int16_t),
              reinterpret_cast<std::byte*>(out.data()),
              out.size() * sizeof(int16_t), bell::audio::SampleRate::SR_44100HZ);
  return out;
}

// Over the second half only, past the filters' settling.
int peak(const std::vector<int16_t>& pcm, size_t channel) {
  int p = 0;
  for (size_t i = kFrames / 2; i < kFrames; i++) {
    p = std::max(p, std::abs(static_cast<int>(pcm[2 * i + channel])));
  }
  return p;
}

int railHits(const std::vector<int16_t>& pcm) {
  int hits = 0;
  for (int16_t v : pcm) {
    hits += (v == 32767 || v == -32768) ? 1 : 0;
  }
  return hits;
}

float db(int out, int in) {
  return 20.0f * std::log10(static_cast<float>(out) / static_cast<float>(in));
}

BiquadParameters shelf(BiquadParameters::Type type, float hz, float gainDb) {
  return BiquadParameters(type, hz, 0.707f, gainDb, std::nullopt,
                          std::nullopt);
}

void testPeakGain() {
  CHECK(biquadPeakGainDb({}) == 0.0f);
  const float bass =
      biquadPeakGainDb({shelf(BiquadParameters::Type::Lowshelf, 150, 6)});
  CHECK(std::fabs(bass - 6.0f) < 0.1f);
  const float both = biquadPeakGainDb(
      {shelf(BiquadParameters::Type::Lowshelf, 150, 6),
       shelf(BiquadParameters::Type::Highshelf, 8000, 3)});
  CHECK(std::fabs(both - 6.0f) < 0.1f);
  const float cut =
      biquadPeakGainDb({shelf(BiquadParameters::Type::Lowshelf, 150, -6)});
  CHECK(cut == 0.0f);
  const float highpass = biquadPeakGainDb({BiquadParameters(
      BiquadParameters::Type::Highpass, 500, 0.707f, std::nullopt,
      std::nullopt, std::nullopt)});
  CHECK(highpass < 0.05f);
}

void testStereoIsTransparent() {
  const auto in = tone(1000, 30000);
  const auto out = run(DspFlow::Stereo, {}, in);
  CHECK(out == in);
}

void testBothChannelsFiltered() {
  const auto in = tone(50, 8000);
  const auto out = run(DspFlow::EQBassTreble, {150, 6, 8000, 3}, in);
  CHECK(peak(out, 0) == peak(out, 1));
}

void testFullScaleBoostDoesNotClip() {
  const auto in = tone(50, 32767);
  const auto out = run(DspFlow::BassBoost, {150, 9, 0, 0}, in);
  CHECK(railHits(out) == 0);
  // The boost lands on the bass, a little short of the headroom taken.
  const float gain = db(peak(out, 0), peak(in, 0));
  CHECK(gain < 0.0f && gain > -1.5f);
}

void testHeadroomLowersTheRest() {
  const auto in = tone(3000, 8000);
  const auto out = run(DspFlow::BassBoost, {150, 9, 0, 0}, in);
  CHECK(std::fabs(db(peak(out, 0), peak(in, 0)) + 9.0f) < 0.2f);
}

void testCrossoverNeedsNoHeadroom() {
  const auto in = tone(50, 8000);
  const auto out = run(DspFlow::Biamp, {500, 0, 500, 0}, in);
  CHECK(std::abs(db(peak(out, 0), peak(in, 0))) < 0.2f);
  CHECK(peak(out, 1) < peak(in, 1) / 10);
}

}  // namespace
}  // namespace snapclient

int main() {
  snapclient::testPeakGain();
  snapclient::testStereoIsTransparent();
  snapclient::testBothChannelsFiltered();
  snapclient::testFullScaleBoostDoesNotClip();
  snapclient::testHeadroomLowersTheRest();
  snapclient::testCrossoverNeedsNoHeadroom();
  if (gFailures == 0) {
    std::printf("dsp_processor_test: all passed\n");
  }
  return gFailures == 0 ? 0 : 1;
}
