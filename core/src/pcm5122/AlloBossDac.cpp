#include "snapclient/pcm5122/AlloBossDac.h"

#include <chrono>
#include <thread>

namespace snapclient {

namespace {

constexpr uint8_t kGpio3Osc48k = 0x04;
constexpr uint8_t kGpio4 = 0x08;
constexpr uint8_t kGpio6Osc44k1 = 0x20;

constexpr uint32_t kOsc48kHz = 49'152'000;
constexpr uint32_t kOsc44k1Hz = 45'158'400;

// 16-bit stereo.
constexpr uint32_t kBckPerFrame = 32;

// What Linux waits before reading the clock detector.
constexpr auto kSckSettle = std::chrono::milliseconds(2);

}  // namespace

AlloBossDac::AlloBossDac(Pcm5122Driver& dac) : dac_(dac) {}

bool AlloBossDac::reset() {
  return dac_.reset() && dac_.setStandby(true);
}

bool AlloBossDac::start(uint32_t sampleRate) {
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

  return dac_.configureMaster(*dividers) && dac_.setStandby(false);
}

bool AlloBossDac::startSlave() {
  return dac_.setStandby(true) &&
         dac_.enableRegisterGpios(kGpio3Osc48k | kGpio4 | kGpio6Osc44k1) &&
         dac_.setGpioLevels(kGpio4) && dac_.configureSlave() &&
         dac_.setStandby(false);
}

bool AlloBossDac::setMuted(bool muted) {
  return dac_.setMuted(muted);
}

bool AlloBossDac::setVolumeCode(uint8_t code) {
  return dac_.setVolumeCode(code);
}

}  // namespace snapclient
