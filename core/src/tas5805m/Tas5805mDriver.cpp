#include "snapclient/tas5805m/Tas5805mDriver.h"

#include <cmath>
#include <cstring>

namespace snapclient {

namespace {

constexpr uint8_t kRegPageSet = 0x00;
constexpr uint8_t kRegBookSet = 0x7f;
constexpr uint8_t kBookControlPort = 0x00;
constexpr uint8_t kPageZero = 0x00;

constexpr uint8_t kRegDeviceCtrl1 = 0x02;
constexpr uint8_t kRegDeviceCtrl2 = 0x03;
constexpr uint8_t kRegDigVolCtrl = 0x4c;
constexpr uint8_t kRegAnaCtrl = 0x53;
constexpr uint8_t kRegAgain = 0x54;
constexpr uint8_t kRegChanFault = 0x70;
constexpr uint8_t kRegGlobalFault1 = 0x71;
constexpr uint8_t kRegGlobalFault2 = 0x72;
constexpr uint8_t kRegOtWarning = 0x73;
constexpr uint8_t kRegFaultClear = 0x78;
constexpr uint8_t kAnalogFaultClear = 0x80;

constexpr uint8_t kBook5 = 0x8c;
constexpr uint8_t kBook5MixerPage = 0x29;
constexpr uint8_t kRegLeftToLeftGain = 0x18;
constexpr uint8_t kRegRightToLeftGain = 0x1c;
constexpr uint8_t kRegLeftToRightGain = 0x20;
constexpr uint8_t kRegRightToRightGain = 0x24;

constexpr uint8_t kBook5VolumePage = 0x2a;
constexpr uint8_t kRegLeftVolume = 0x24;
constexpr uint8_t kRegRightVolume = 0x28;

// Pre-encoded on-wire values; no endian swap needed.
constexpr uint32_t kMixerValueMute = 0x00000000;
constexpr uint32_t kMixerValue0dB = 0x00008000;
constexpr uint32_t kMixerValueMinus6dB = 0x00004000;

constexpr uint8_t kAnalogGainTable[32] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
    0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
    0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
};

uint32_t swapEndian32(uint32_t val) {
  return ((val & 0xFF) << 24) | ((val & 0xFF00) << 8) |
         ((val & 0xFF0000) >> 8) | ((val >> 24) & 0xFF);
}

uint32_t floatToQ9_23(float value) {
  if (value > 255.999999f) value = 255.999999f;
  if (value < -256.0f) value = -256.0f;
  auto fixedVal = static_cast<int32_t>(value * static_cast<float>(1 << 23));
  return swapEndian32(static_cast<uint32_t>(fixedVal));
}

}  // namespace

bool Tas5805mFaults::any() const {
  return rightOverCurrent || leftOverCurrent || rightDcFault || leftDcFault ||
         pvddUnderVoltage || pvddOverVoltage || clockFault ||
         biquadWriteFailed || otpCrcError || overTemperatureShutdown ||
         overTemperatureWarning;
}

Tas5805mDriver::Tas5805mDriver(I2cBus& bus, uint8_t deviceAddr)
    : bus_(bus), deviceAddr_(deviceAddr) {}

bool Tas5805mDriver::writeByte(uint8_t reg, uint8_t value) {
  uint8_t buf[2] = {reg, value};
  return bus_.write(deviceAddr_, buf, sizeof(buf));
}

bool Tas5805mDriver::readByte(uint8_t reg, uint8_t& value) {
  return bus_.writeThenRead(deviceAddr_, &reg, 1, &value, 1);
}

bool Tas5805mDriver::writeRegBytes(uint8_t reg, const uint8_t* data,
                                   size_t len) {
  uint8_t buf[5];
  buf[0] = reg;
  std::memcpy(buf + 1, data, len);
  return bus_.write(deviceAddr_, buf, len + 1);
}

void Tas5805mDriver::setBookAndPage(uint8_t book, uint8_t page) {
  writeByte(kRegPageSet, kPageZero);
  writeByte(kRegBookSet, book);
  writeByte(kRegPageSet, page);
}

bool Tas5805mDriver::setState(Tas5805mState state) {
  std::scoped_lock lock(mutex_);
  return writeByte(kRegDeviceCtrl2, static_cast<uint8_t>(state));
}

bool Tas5805mDriver::setDigitalVolume(uint8_t volume) {
  std::scoped_lock lock(mutex_);
  return writeByte(kRegDigVolCtrl, volume);
}

bool Tas5805mDriver::getDigitalVolume(uint8_t& volume) {
  std::scoped_lock lock(mutex_);
  return readByte(kRegDigVolCtrl, volume);
}

bool Tas5805mDriver::setAnalogGain(uint8_t gain) {
  if (gain > 31) {
    return false;
  }
  std::scoped_lock lock(mutex_);
  return writeByte(kRegAgain, kAnalogGainTable[gain]);
}

bool Tas5805mDriver::getAnalogGain(uint8_t& gain) {
  std::scoped_lock lock(mutex_);
  return readByte(kRegAgain, gain);
}

bool Tas5805mDriver::setDacMode(Tas5805mDacMode mode) {
  std::scoped_lock lock(mutex_);
  uint8_t current = 0;
  if (!readByte(kRegDeviceCtrl1, current)) {
    return false;
  }
  if (mode == Tas5805mDacMode::Pbtl) {
    current |= (1 << 2);
  } else {
    current &= ~(1 << 2);
  }
  return writeByte(kRegDeviceCtrl1, current);
}

bool Tas5805mDriver::getDacMode(Tas5805mDacMode& mode) {
  std::scoped_lock lock(mutex_);
  uint8_t current = 0;
  if (!readByte(kRegDeviceCtrl1, current)) {
    return false;
  }
  mode = (current & (1 << 2)) ? Tas5805mDacMode::Pbtl : Tas5805mDacMode::Btl;
  return true;
}

bool Tas5805mDriver::setModulationMode(Tas5805mModMode mode,
                                       Tas5805mSwFreq swFreq,
                                       Tas5805mBdFreq bdFreq) {
  std::scoped_lock lock(mutex_);
  uint8_t current = 0;
  if (!readByte(kRegDeviceCtrl1, current)) {
    return false;
  }
  current &= ~((0x07 << 4) | 0x03);
  current |= static_cast<uint8_t>(mode) & 0x03;
  current |= static_cast<uint8_t>(swFreq) & 0x70;
  if (!writeByte(kRegDeviceCtrl1, current)) {
    return false;
  }
  return writeByte(kRegAnaCtrl, static_cast<uint8_t>(bdFreq));
}

bool Tas5805mDriver::getModulationMode(Tas5805mModMode& mode,
                                       Tas5805mSwFreq& swFreq,
                                       Tas5805mBdFreq& bdFreq) {
  std::scoped_lock lock(mutex_);
  uint8_t current = 0;
  if (!readByte(kRegDeviceCtrl1, current)) {
    return false;
  }
  mode = static_cast<Tas5805mModMode>(current & 0x03);
  swFreq = static_cast<Tas5805mSwFreq>(current & 0x70);

  if (!readByte(kRegAnaCtrl, current)) {
    return false;
  }
  bdFreq = static_cast<Tas5805mBdFreq>(current & 0x60);
  return true;
}

bool Tas5805mDriver::setMixerGain(uint8_t reg, uint32_t rawGain) {
  setBookAndPage(kBook5, kBook5MixerPage);
  bool ok = writeRegBytes(reg, reinterpret_cast<const uint8_t*>(&rawGain),
                          sizeof(rawGain));
  setBookAndPage(kBookControlPort, kPageZero);
  return ok;
}

bool Tas5805mDriver::setMixerMode(Tas5805mMixerMode mode) {
  std::scoped_lock lock(mutex_);

  uint32_t l2l, r2r, l2r, r2l;
  switch (mode) {
    case Tas5805mMixerMode::Stereo:
      l2l = kMixerValue0dB;
      r2r = kMixerValue0dB;
      l2r = kMixerValueMute;
      r2l = kMixerValueMute;
      break;
    case Tas5805mMixerMode::StereoInverse:
      l2l = kMixerValueMute;
      r2r = kMixerValueMute;
      l2r = kMixerValue0dB;
      r2l = kMixerValue0dB;
      break;
    case Tas5805mMixerMode::Mono:
      l2l = kMixerValueMinus6dB;
      r2r = kMixerValueMinus6dB;
      l2r = kMixerValueMinus6dB;
      r2l = kMixerValueMinus6dB;
      break;
    case Tas5805mMixerMode::Left:
      l2l = kMixerValue0dB;
      r2r = kMixerValueMute;
      l2r = kMixerValue0dB;
      r2l = kMixerValueMute;
      break;
    case Tas5805mMixerMode::Right:
      l2l = kMixerValueMute;
      r2r = kMixerValue0dB;
      l2r = kMixerValueMute;
      r2l = kMixerValue0dB;
      break;
    default:
      return false;
  }

  bool ok = setMixerGain(kRegLeftToLeftGain, l2l);
  ok = setMixerGain(kRegRightToRightGain, r2r) && ok;
  ok = setMixerGain(kRegLeftToRightGain, l2r) && ok;
  ok = setMixerGain(kRegRightToLeftGain, r2l) && ok;
  return ok;
}

bool Tas5805mDriver::setChannelGain(Tas5805mChannel channel, int8_t gainDb) {
  if (gainDb < -24 || gainDb > 24) {
    return false;
  }
  std::scoped_lock lock(mutex_);

  float linear = powf(10.0f, static_cast<float>(gainDb) / 20.0f);
  uint32_t regValue = floatToQ9_23(linear);
  uint8_t reg =
      (channel == Tas5805mChannel::Right) ? kRegRightVolume : kRegLeftVolume;

  setBookAndPage(kBook5, kBook5VolumePage);
  bool ok = writeRegBytes(reg, reinterpret_cast<const uint8_t*>(&regValue),
                          sizeof(regValue));
  setBookAndPage(kBookControlPort, kPageZero);
  return ok;
}

bool Tas5805mDriver::getFaults(Tas5805mFaults& faults) {
  std::scoped_lock lock(mutex_);
  uint8_t err0 = 0, err1 = 0, err2 = 0, otWarn = 0;
  if (!readByte(kRegChanFault, err0) || !readByte(kRegGlobalFault1, err1) ||
      !readByte(kRegGlobalFault2, err2) || !readByte(kRegOtWarning, otWarn)) {
    return false;
  }

  faults.rightOverCurrent = err0 & (1 << 0);
  faults.leftOverCurrent = err0 & (1 << 1);
  faults.rightDcFault = err0 & (1 << 2);
  faults.leftDcFault = err0 & (1 << 3);

  faults.pvddUnderVoltage = err1 & (1 << 0);
  faults.pvddOverVoltage = err1 & (1 << 1);
  faults.clockFault = err1 & (1 << 2);
  faults.biquadWriteFailed = err1 & (1 << 6);
  faults.otpCrcError = err1 & (1 << 7);

  faults.overTemperatureShutdown = err2 & (1 << 0);
  faults.overTemperatureWarning = otWarn & (1 << 2);

  return true;
}

bool Tas5805mDriver::clearFaults() {
  std::scoped_lock lock(mutex_);
  return writeByte(kRegFaultClear, kAnalogFaultClear);
}

}  // namespace snapclient
