#pragma once

#include <cstdint>
#include <mutex>

#include "snapclient/I2cBus.h"

namespace snapclient {

enum class Tas5805mState : uint8_t {
  DeepSleep = 0x00,
  Sleep = 0x01,
  HiZ = 0x02,
  Play = 0x03,
  PlayMute = 0x0B,
};

enum class Tas5805mDacMode : uint8_t { Btl = 0, Pbtl = 1 };

enum class Tas5805mModMode : uint8_t { Bd = 0, OneSpw = 1, Hybrid = 2 };

enum class Tas5805mSwFreq : uint8_t {
  Khz768 = 0x00 << 4,
  Khz384 = 0x01 << 4,
  Khz480 = 0x03 << 4,
  Khz576 = 0x04 << 4,
};

enum class Tas5805mBdFreq : uint8_t {
  Khz80 = 0x00 << 5,
  Khz100 = 0x01 << 5,
  Khz120 = 0x02 << 5,
  Khz175 = 0x03 << 5,
};

enum class Tas5805mMixerMode { Stereo, StereoInverse, Mono, Left, Right };

enum class Tas5805mChannel { Left, Right };

struct Tas5805mFaults {
  bool rightOverCurrent = false;
  bool leftOverCurrent = false;
  bool rightDcFault = false;
  bool leftDcFault = false;
  bool pvddUnderVoltage = false;
  bool pvddOverVoltage = false;
  bool clockFault = false;
  bool biquadWriteFailed = false;
  bool otpCrcError = false;
  bool overTemperatureShutdown = false;
  bool overTemperatureWarning = false;

  bool any() const;
};

// Register/book-page logic for the TAS5805M's non-EQ controls (state,
// volume, analog gain, DAC mode, modulation, mixer routing, per-channel
// gain, faults) - book 0xaa (the 15-band EQ) is untouched. Owns no GPIO;
// PDN/reset handling stays with the caller.
class Tas5805mDriver {
 public:
  explicit Tas5805mDriver(I2cBus& bus, uint8_t deviceAddr = 0x2D);

  bool setState(Tas5805mState state);

  bool setDigitalVolume(uint8_t volume);  // 0-255, 0 mutes
  bool getDigitalVolume(uint8_t& volume);

  bool setAnalogGain(uint8_t gain);  // raw index 0-31, 0 = 0dB
  bool getAnalogGain(uint8_t& gain);

  bool setDacMode(Tas5805mDacMode mode);
  bool getDacMode(Tas5805mDacMode& mode);

  bool setModulationMode(Tas5805mModMode mode, Tas5805mSwFreq swFreq,
                         Tas5805mBdFreq bdFreq);
  bool getModulationMode(Tas5805mModMode& mode, Tas5805mSwFreq& swFreq,
                         Tas5805mBdFreq& bdFreq);

  bool setMixerMode(Tas5805mMixerMode mode);

  bool setChannelGain(Tas5805mChannel channel, int8_t gainDb);  // -24..24

  bool getFaults(Tas5805mFaults& faults);
  bool clearFaults();

 private:
  I2cBus& bus_;
  uint8_t deviceAddr_;
  std::mutex mutex_;

  bool writeByte(uint8_t reg, uint8_t value);
  bool readByte(uint8_t reg, uint8_t& value);
  bool writeRegBytes(uint8_t reg, const uint8_t* data, size_t len);
  void setBookAndPage(uint8_t book, uint8_t page);
  bool setMixerGain(uint8_t reg, uint32_t rawGain);
};

}  // namespace snapclient
