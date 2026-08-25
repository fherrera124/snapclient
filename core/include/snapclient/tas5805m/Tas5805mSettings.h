#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <string>

#include "snapclient/SettingsStore.h"
#include "snapclient/tas5805m/Tas5805mDriver.h"

namespace snapclient {

// Typed settings model over a SettingsStore for the TAS5805M's persisted
// controls: mutations persist immediately, pure data - no I2C, no
// dependency on Tas5805mDriver. state/digital_volume are deliberately not
// modeled here (app-owned, not persisted).
class Tas5805mSettings {
 public:
  explicit Tas5805mSettings(SettingsStore& store);

  uint8_t analogGain() const;      // 0-31
  void setAnalogGain(uint8_t gain);

  Tas5805mDacMode dacMode() const;
  void setDacMode(Tas5805mDacMode mode);

  Tas5805mModMode modulationMode() const;
  Tas5805mSwFreq swFreq() const;
  Tas5805mBdFreq bdFreq() const;
  void setModulation(Tas5805mModMode mode, Tas5805mSwFreq swFreq,
                     Tas5805mBdFreq bdFreq);

  Tas5805mMixerMode mixerMode() const;
  void setMixerMode(Tas5805mMixerMode mode);

  int8_t channelGainLeft() const;   // -24..24
  int8_t channelGainRight() const;  // -24..24
  void setChannelGain(int8_t left, int8_t right);

  Tas5805mEqMode eqMode() const;
  void setEqMode(Tas5805mEqMode mode);

  std::array<int8_t, 15> eqGainLeft() const;   // -15..15 per band
  std::array<int8_t, 15> eqGainRight() const;  // -15..15 per band
  void setEqGain(const std::array<int8_t, 15>& left,
                const std::array<int8_t, 15>& right);

  Tas5805mEqProfile eqProfileLeft() const;
  Tas5805mEqProfile eqProfileRight() const;
  void setEqProfile(Tas5805mEqProfile left, Tas5805mEqProfile right);

  std::string toJson() const;

  // Validates the whole partial payload before applying anything: false
  // (no changes made) on a parse error, wrong field type, unknown enum
  // name, or out-of-range value.
  bool applyJson(const std::string& json);

 private:
  SettingsStore& store_;
  mutable std::mutex mutex_;

  uint8_t analogGain_ = 0;
  Tas5805mDacMode dacMode_ = Tas5805mDacMode::Btl;
  Tas5805mModMode modMode_ = Tas5805mModMode::Bd;
  Tas5805mSwFreq swFreq_ = Tas5805mSwFreq::Khz768;
  Tas5805mBdFreq bdFreq_ = Tas5805mBdFreq::Khz80;
  Tas5805mMixerMode mixerMode_ = Tas5805mMixerMode::Stereo;
  int8_t channelGainL_ = 0;
  int8_t channelGainR_ = 0;
  Tas5805mEqMode eqMode_ = Tas5805mEqMode::Off;
  std::array<int8_t, 15> eqGainL_{};
  std::array<int8_t, 15> eqGainR_{};
  Tas5805mEqProfile eqProfileL_ = Tas5805mEqProfile::Flat;
  Tas5805mEqProfile eqProfileR_ = Tas5805mEqProfile::Flat;

  void load();
};

}  // namespace snapclient
