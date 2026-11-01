#include "snapclient/pcm5122/AlloBossDac.h"

#include <chrono>
#include <thread>

#include <bell/Logger.h>

namespace snapclient {

namespace {

const char* kLogTag = "AlloBossDac";

constexpr uint8_t kGpio3Osc48k = 0x04;
constexpr uint8_t kGpio4 = 0x08;
constexpr uint8_t kGpio6Osc44k1 = 0x20;

constexpr uint32_t kOsc48kHz = 49'152'000;
constexpr uint32_t kOsc44k1Hz = 45'158'400;

// 32-bit stereo.
constexpr uint32_t kBckPerFrame = 64;

// What Linux waits before reading the clock detector.
constexpr auto kSckSettle = std::chrono::milliseconds(2);

constexpr uint8_t kProgramPlain = 1;
constexpr uint8_t kProgramEq = 5;

}  // namespace

AlloBossDac::AlloBossDac(Pcm5122Driver& dac) : dac_(dac) {}

bool AlloBossDac::reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  sampleRate_ = 0;
  imageLoaded_ = false;
  loadedEq_.reset();
  eqKnown_ = false;
  return dac_.reset() && dac_.setStandby(true);
}

bool AlloBossDac::start(uint32_t sampleRate) {
  std::lock_guard<std::mutex> lock(mutex_);
  sampleRate_ = 0;
  if (!dac_.setStandby(true) || sampleRate == 0) {
    return false;
  }

  const bool family44k1 = kOsc44k1Hz % sampleRate == 0;
  const uint8_t osc = family44k1 ? kGpio6Osc44k1 : kGpio3Osc48k;
  const uint32_t sckHz = family44k1 ? kOsc44k1Hz : kOsc48kHz;
  const auto dividers =
      pcm5122MasterDividers(sckHz, sampleRate, kBckPerFrame);
  if (!dividers) {
    return false;
  }

  if (!dac_.enableRegisterGpios(kGpio3Osc48k | kGpio4 | kGpio6Osc44k1) ||
      !dac_.setGpioLevels(osc | kGpio4)) {
    return false;
  }

  std::this_thread::sleep_for(kSckSettle);
  bool sck = false;
  if (!dac_.sckPresent(sck) || !sck) {
    return false;
  }

  if (!dac_.configureMaster(*dividers)) {
    return false;
  }
  sampleRate_ = sampleRate;
  if (!loadEq(wantedEq())) {
    BELL_LOG(error, kLogTag, "PCM5122 EQ load failed");
  }
  return dac_.setStandby(false);
}

bool AlloBossDac::startSlave() {
  std::lock_guard<std::mutex> lock(mutex_);
  sampleRate_ = 0;
  return dac_.setStandby(true) &&
         dac_.enableRegisterGpios(kGpio3Osc48k | kGpio4 | kGpio6Osc44k1) &&
         dac_.setGpioLevels(kGpio4) && dac_.configureSlave() &&
         dac_.setStandby(false);
}

bool AlloBossDac::setMuted(bool muted) {
  std::lock_guard<std::mutex> lock(mutex_);
  return dac_.setMuted(muted);
}

bool AlloBossDac::setVolumeCode(uint8_t code) {
  std::lock_guard<std::mutex> lock(mutex_);
  return dac_.setVolumeCode(code);
}

bool AlloBossDac::setEq(std::vector<bell::dsp::BiquadParameters> stages) {
  std::lock_guard<std::mutex> lock(mutex_);
  eqStages_ = std::move(stages);
  if (sampleRate_ == 0) {
    return eqStages_.empty();
  }
  const auto eq = wantedEq();
  if (!eqKnown_ || eq != loadedEq_) {
    const bool loaded = dac_.setStandby(true) && loadEq(eq);
    if (!dac_.setStandby(false) || !loaded) {
      BELL_LOG(error, kLogTag, "PCM5122 EQ change failed");
    }
  }
  if (eqStages_.empty()) {
    return eqKnown_ && !loadedEq_;
  }
  return eqKnown_ && eq && loadedEq_ == eq;
}

std::optional<Pcm5122Eq> AlloBossDac::wantedEq() const {
  if (eqStages_.empty() || sampleRate_ > kPcm5122Program5MaxRate) {
    return std::nullopt;
  }
  return pcm5122Eq(eqStages_, sampleRate_);
}

bool AlloBossDac::loadEq(const std::optional<Pcm5122Eq>& eq) {
  if (eq && !imageLoaded_) {
    imageLoaded_ = dac_.writeCoefficients(kPcm5122Program5ImageFirst,
                                          kPcm5122Program5Image.data(),
                                          kPcm5122Program5Image.size());
  }
  if (eq && imageLoaded_ &&
      dac_.writeCoefficients(Pcm5122Eq::kFirstBiquad, eq->biquads.data(),
                             eq->biquads.size()) &&
      dac_.writeCoefficients(Pcm5122Eq::kVolume, &eq->volume, 1) &&
      dac_.selectProgram(kProgramEq)) {
    loadedEq_ = eq;
    eqKnown_ = true;
    return true;
  }
  loadedEq_.reset();
  eqKnown_ = dac_.selectProgram(kProgramPlain);
  return eqKnown_ && !eq;
}

}  // namespace snapclient
