#include "snapclient/tas5805m/Tas5805mSettings.h"

#include <optional>

#include <tao/json.hpp>

namespace snapclient {

namespace {

const char* kAnalogGainKey = "dac.analogGain";
const char* kDacModeKey = "dac.dacMode";
const char* kModModeKey = "dac.modMode";
const char* kSwFreqKey = "dac.swFreq";
const char* kBdFreqKey = "dac.bdFreq";
const char* kMixerModeKey = "dac.mixerMode";
const char* kChannelGainLKey = "dac.channelGainL";
const char* kChannelGainRKey = "dac.channelGainR";
const char* kEqModeKey = "dac.eq.mode";
const char* kEqGainLPrefix = "dac.eq.gainL.";
const char* kEqGainRPrefix = "dac.eq.gainR.";
const char* kEqProfileLKey = "dac.eq.profileL";
const char* kEqProfileRKey = "dac.eq.profileR";

const char* dacModeName(Tas5805mDacMode mode) {
  return mode == Tas5805mDacMode::Pbtl ? "pbtl" : "btl";
}

std::optional<Tas5805mDacMode> dacModeFromName(const std::string& name) {
  if (name == "btl") return Tas5805mDacMode::Btl;
  if (name == "pbtl") return Tas5805mDacMode::Pbtl;
  return std::nullopt;
}

const char* modModeName(Tas5805mModMode mode) {
  switch (mode) {
    case Tas5805mModMode::Bd:
      return "bd";
    case Tas5805mModMode::OneSpw:
      return "1spw";
    case Tas5805mModMode::Hybrid:
      return "hybrid";
  }
  return "bd";
}

std::optional<Tas5805mModMode> modModeFromName(const std::string& name) {
  if (name == "bd") return Tas5805mModMode::Bd;
  if (name == "1spw") return Tas5805mModMode::OneSpw;
  if (name == "hybrid") return Tas5805mModMode::Hybrid;
  return std::nullopt;
}

int32_t swFreqHz(Tas5805mSwFreq freq) {
  switch (freq) {
    case Tas5805mSwFreq::Khz768:
      return 768000;
    case Tas5805mSwFreq::Khz384:
      return 384000;
    case Tas5805mSwFreq::Khz480:
      return 480000;
    case Tas5805mSwFreq::Khz576:
      return 576000;
  }
  return 768000;
}

std::optional<Tas5805mSwFreq> swFreqFromHz(int32_t hz) {
  switch (hz) {
    case 768000:
      return Tas5805mSwFreq::Khz768;
    case 384000:
      return Tas5805mSwFreq::Khz384;
    case 480000:
      return Tas5805mSwFreq::Khz480;
    case 576000:
      return Tas5805mSwFreq::Khz576;
    default:
      return std::nullopt;
  }
}

int32_t bdFreqHz(Tas5805mBdFreq freq) {
  switch (freq) {
    case Tas5805mBdFreq::Khz80:
      return 80000;
    case Tas5805mBdFreq::Khz100:
      return 100000;
    case Tas5805mBdFreq::Khz120:
      return 120000;
    case Tas5805mBdFreq::Khz175:
      return 175000;
  }
  return 80000;
}

std::optional<Tas5805mBdFreq> bdFreqFromHz(int32_t hz) {
  switch (hz) {
    case 80000:
      return Tas5805mBdFreq::Khz80;
    case 100000:
      return Tas5805mBdFreq::Khz100;
    case 120000:
      return Tas5805mBdFreq::Khz120;
    case 175000:
      return Tas5805mBdFreq::Khz175;
    default:
      return std::nullopt;
  }
}

const char* mixerModeName(Tas5805mMixerMode mode) {
  switch (mode) {
    case Tas5805mMixerMode::Stereo:
      return "stereo";
    case Tas5805mMixerMode::StereoInverse:
      return "stereoInverse";
    case Tas5805mMixerMode::Mono:
      return "mono";
    case Tas5805mMixerMode::Left:
      return "left";
    case Tas5805mMixerMode::Right:
      return "right";
  }
  return "stereo";
}

std::optional<Tas5805mMixerMode> mixerModeFromName(const std::string& name) {
  if (name == "stereo") return Tas5805mMixerMode::Stereo;
  if (name == "stereoInverse") return Tas5805mMixerMode::StereoInverse;
  if (name == "mono") return Tas5805mMixerMode::Mono;
  if (name == "left") return Tas5805mMixerMode::Left;
  if (name == "right") return Tas5805mMixerMode::Right;
  return std::nullopt;
}

bool inRange(int32_t v, int32_t lo, int32_t hi) { return v >= lo && v <= hi; }

const char* eqModeName(Tas5805mEqMode mode) {
  switch (mode) {
    case Tas5805mEqMode::Off:
      return "off";
    case Tas5805mEqMode::On:
      return "on";
    case Tas5805mEqMode::Biamp:
      return "biamp";
    case Tas5805mEqMode::BiampOff:
      return "biampOff";
  }
  return "off";
}

std::optional<Tas5805mEqMode> eqModeFromName(const std::string& name) {
  if (name == "off") return Tas5805mEqMode::Off;
  if (name == "on") return Tas5805mEqMode::On;
  if (name == "biamp") return Tas5805mEqMode::Biamp;
  if (name == "biampOff") return Tas5805mEqMode::BiampOff;
  return std::nullopt;
}

constexpr const char* kEqProfileNames[] = {
    "flat",  "lf60",  "lf70",  "lf80",  "lf90",  "lf100", "lf110",
    "lf120", "lf130", "lf140", "lf150", "hf60",  "hf70",  "hf80",
    "hf90",  "hf100", "hf110", "hf120", "hf130", "hf140", "hf150",
};

const char* eqProfileName(Tas5805mEqProfile profile) {
  return kEqProfileNames[static_cast<size_t>(profile)];
}

std::optional<Tas5805mEqProfile> eqProfileFromName(const std::string& name) {
  for (size_t i = 0; i < std::size(kEqProfileNames); i++) {
    if (name == kEqProfileNames[i]) {
      return static_cast<Tas5805mEqProfile>(i);
    }
  }
  return std::nullopt;
}

}  // namespace

Tas5805mSettings::Tas5805mSettings(SettingsStore& store) : store_(store) {
  load();
}

void Tas5805mSettings::load() {
  if (auto v = store_.getInt(kAnalogGainKey)) {
    if (inRange(*v, 0, 31)) {
      analogGain_ = static_cast<uint8_t>(*v);
    }
  }
  if (auto v = store_.getString(kDacModeKey)) {
    if (auto mode = dacModeFromName(*v)) {
      dacMode_ = *mode;
    }
  }
  if (auto v = store_.getString(kModModeKey)) {
    if (auto mode = modModeFromName(*v)) {
      modMode_ = *mode;
    }
  }
  if (auto v = store_.getInt(kSwFreqKey)) {
    if (auto freq = swFreqFromHz(*v)) {
      swFreq_ = *freq;
    }
  }
  if (auto v = store_.getInt(kBdFreqKey)) {
    if (auto freq = bdFreqFromHz(*v)) {
      bdFreq_ = *freq;
    }
  }
  if (auto v = store_.getString(kMixerModeKey)) {
    if (auto mode = mixerModeFromName(*v)) {
      mixerMode_ = *mode;
    }
  }
  if (auto v = store_.getInt(kChannelGainLKey)) {
    if (inRange(*v, -24, 24)) {
      channelGainL_ = static_cast<int8_t>(*v);
    }
  }
  if (auto v = store_.getInt(kChannelGainRKey)) {
    if (inRange(*v, -24, 24)) {
      channelGainR_ = static_cast<int8_t>(*v);
    }
  }
  if (auto v = store_.getString(kEqModeKey)) {
    if (auto mode = eqModeFromName(*v)) {
      eqMode_ = *mode;
    }
  }
  for (size_t i = 0; i < eqGainL_.size(); i++) {
    if (auto v = store_.getInt(kEqGainLPrefix + std::to_string(i))) {
      if (inRange(*v, -15, 15)) {
        eqGainL_[i] = static_cast<int8_t>(*v);
      }
    }
    if (auto v = store_.getInt(kEqGainRPrefix + std::to_string(i))) {
      if (inRange(*v, -15, 15)) {
        eqGainR_[i] = static_cast<int8_t>(*v);
      }
    }
  }
  if (auto v = store_.getString(kEqProfileLKey)) {
    if (auto profile = eqProfileFromName(*v)) {
      eqProfileL_ = *profile;
    }
  }
  if (auto v = store_.getString(kEqProfileRKey)) {
    if (auto profile = eqProfileFromName(*v)) {
      eqProfileR_ = *profile;
    }
  }
}

uint8_t Tas5805mSettings::analogGain() const {
  std::scoped_lock lock(mutex_);
  return analogGain_;
}

void Tas5805mSettings::setAnalogGain(uint8_t gain) {
  std::scoped_lock lock(mutex_);
  analogGain_ = gain;
  store_.setInt(kAnalogGainKey, gain);
}

Tas5805mDacMode Tas5805mSettings::dacMode() const {
  std::scoped_lock lock(mutex_);
  return dacMode_;
}

void Tas5805mSettings::setDacMode(Tas5805mDacMode mode) {
  std::scoped_lock lock(mutex_);
  dacMode_ = mode;
  store_.setString(kDacModeKey, dacModeName(mode));
}

Tas5805mModMode Tas5805mSettings::modulationMode() const {
  std::scoped_lock lock(mutex_);
  return modMode_;
}

Tas5805mSwFreq Tas5805mSettings::swFreq() const {
  std::scoped_lock lock(mutex_);
  return swFreq_;
}

Tas5805mBdFreq Tas5805mSettings::bdFreq() const {
  std::scoped_lock lock(mutex_);
  return bdFreq_;
}

void Tas5805mSettings::setModulation(Tas5805mModMode mode,
                                     Tas5805mSwFreq swFreq,
                                     Tas5805mBdFreq bdFreq) {
  std::scoped_lock lock(mutex_);
  modMode_ = mode;
  swFreq_ = swFreq;
  bdFreq_ = bdFreq;
  store_.setString(kModModeKey, modModeName(mode));
  store_.setInt(kSwFreqKey, swFreqHz(swFreq));
  store_.setInt(kBdFreqKey, bdFreqHz(bdFreq));
}

Tas5805mMixerMode Tas5805mSettings::mixerMode() const {
  std::scoped_lock lock(mutex_);
  return mixerMode_;
}

void Tas5805mSettings::setMixerMode(Tas5805mMixerMode mode) {
  std::scoped_lock lock(mutex_);
  mixerMode_ = mode;
  store_.setString(kMixerModeKey, mixerModeName(mode));
}

int8_t Tas5805mSettings::channelGainLeft() const {
  std::scoped_lock lock(mutex_);
  return channelGainL_;
}

int8_t Tas5805mSettings::channelGainRight() const {
  std::scoped_lock lock(mutex_);
  return channelGainR_;
}

void Tas5805mSettings::setChannelGain(int8_t left, int8_t right) {
  std::scoped_lock lock(mutex_);
  channelGainL_ = left;
  channelGainR_ = right;
  store_.setInt(kChannelGainLKey, left);
  store_.setInt(kChannelGainRKey, right);
}

Tas5805mEqMode Tas5805mSettings::eqMode() const {
  std::scoped_lock lock(mutex_);
  return eqMode_;
}

void Tas5805mSettings::setEqMode(Tas5805mEqMode mode) {
  std::scoped_lock lock(mutex_);
  eqMode_ = mode;
  store_.setString(kEqModeKey, eqModeName(mode));
}

std::array<int8_t, 15> Tas5805mSettings::eqGainLeft() const {
  std::scoped_lock lock(mutex_);
  return eqGainL_;
}

std::array<int8_t, 15> Tas5805mSettings::eqGainRight() const {
  std::scoped_lock lock(mutex_);
  return eqGainR_;
}

void Tas5805mSettings::setEqGain(const std::array<int8_t, 15>& left,
                                 const std::array<int8_t, 15>& right) {
  std::scoped_lock lock(mutex_);
  eqGainL_ = left;
  eqGainR_ = right;
  for (size_t i = 0; i < eqGainL_.size(); i++) {
    store_.setInt(kEqGainLPrefix + std::to_string(i), eqGainL_[i]);
    store_.setInt(kEqGainRPrefix + std::to_string(i), eqGainR_[i]);
  }
}

Tas5805mEqProfile Tas5805mSettings::eqProfileLeft() const {
  std::scoped_lock lock(mutex_);
  return eqProfileL_;
}

Tas5805mEqProfile Tas5805mSettings::eqProfileRight() const {
  std::scoped_lock lock(mutex_);
  return eqProfileR_;
}

void Tas5805mSettings::setEqProfile(Tas5805mEqProfile left,
                                    Tas5805mEqProfile right) {
  std::scoped_lock lock(mutex_);
  eqProfileL_ = left;
  eqProfileR_ = right;
  store_.setString(kEqProfileLKey, eqProfileName(left));
  store_.setString(kEqProfileRKey, eqProfileName(right));
}

std::string Tas5805mSettings::toJson() const {
  std::scoped_lock lock(mutex_);

  tao::json::value obj;
  obj["dac"]["analogGain"] = analogGain_;
  obj["dac"]["dacMode"] = dacModeName(dacMode_);
  obj["dac"]["modulation"]["mode"] = modModeName(modMode_);
  obj["dac"]["modulation"]["swFreqHz"] = swFreqHz(swFreq_);
  obj["dac"]["modulation"]["bdFreqHz"] = bdFreqHz(bdFreq_);
  obj["dac"]["mixerMode"] = mixerModeName(mixerMode_);
  obj["dac"]["channelGainL"] = channelGainL_;
  obj["dac"]["channelGainR"] = channelGainR_;

  tao::json::value gainL;
  tao::json::value gainR;
  for (size_t i = 0; i < eqGainL_.size(); i++) {
    gainL.emplace_back(eqGainL_[i]);
    gainR.emplace_back(eqGainR_[i]);
  }
  obj["dac"]["eq"]["mode"] = eqModeName(eqMode_);
  obj["dac"]["eq"]["gainL"] = std::move(gainL);
  obj["dac"]["eq"]["gainR"] = std::move(gainR);
  obj["dac"]["eq"]["profileL"] = eqProfileName(eqProfileL_);
  obj["dac"]["eq"]["profileR"] = eqProfileName(eqProfileR_);

  return tao::json::to_string(obj);
}

bool Tas5805mSettings::applyJson(const std::string& json) {
  tao::json::value obj;
  try {
    obj = tao::json::from_string(json);
  } catch (const std::exception&) {
    return false;
  }
  if (!obj.is_object()) {
    return false;
  }

  std::optional<uint8_t> newAnalogGain;
  std::optional<Tas5805mDacMode> newDacMode;
  std::optional<Tas5805mModMode> newModMode;
  std::optional<Tas5805mSwFreq> newSwFreq;
  std::optional<Tas5805mBdFreq> newBdFreq;
  std::optional<Tas5805mMixerMode> newMixerMode;
  std::optional<int8_t> newChannelGainL;
  std::optional<int8_t> newChannelGainR;
  std::optional<Tas5805mEqMode> newEqMode;
  std::optional<std::array<int8_t, 15>> newEqGainL;
  std::optional<std::array<int8_t, 15>> newEqGainR;
  std::optional<Tas5805mEqProfile> newEqProfileL;
  std::optional<Tas5805mEqProfile> newEqProfileR;

  try {
    const auto* dac = obj.find("dac");
    if (!dac) {
      return true;  // Nothing to apply, not an error.
    }
    if (!dac->is_object()) {
      return false;
    }

    if (const auto* v = dac->find("analogGain")) {
      auto gain = v->as<int32_t>();
      if (!inRange(gain, 0, 31)) {
        return false;
      }
      newAnalogGain = static_cast<uint8_t>(gain);
    }
    if (const auto* v = dac->find("dacMode")) {
      auto mode = dacModeFromName(v->as<std::string>());
      if (!mode) {
        return false;
      }
      newDacMode = *mode;
    }
    if (const auto* mod = dac->find("modulation")) {
      if (!mod->is_object()) {
        return false;
      }
      Tas5805mModMode mode = modulationMode();
      Tas5805mSwFreq sw = swFreq();
      Tas5805mBdFreq bd = bdFreq();
      if (const auto* v = mod->find("mode")) {
        auto parsed = modModeFromName(v->as<std::string>());
        if (!parsed) {
          return false;
        }
        mode = *parsed;
      }
      if (const auto* v = mod->find("swFreqHz")) {
        auto parsed = swFreqFromHz(v->as<int32_t>());
        if (!parsed) {
          return false;
        }
        sw = *parsed;
      }
      if (const auto* v = mod->find("bdFreqHz")) {
        auto parsed = bdFreqFromHz(v->as<int32_t>());
        if (!parsed) {
          return false;
        }
        bd = *parsed;
      }
      newModMode = mode;
      newSwFreq = sw;
      newBdFreq = bd;
    }
    if (const auto* v = dac->find("mixerMode")) {
      auto mode = mixerModeFromName(v->as<std::string>());
      if (!mode) {
        return false;
      }
      newMixerMode = *mode;
    }
    if (const auto* v = dac->find("channelGainL")) {
      auto gain = v->as<int32_t>();
      if (!inRange(gain, -24, 24)) {
        return false;
      }
      newChannelGainL = static_cast<int8_t>(gain);
    }
    if (const auto* v = dac->find("channelGainR")) {
      auto gain = v->as<int32_t>();
      if (!inRange(gain, -24, 24)) {
        return false;
      }
      newChannelGainR = static_cast<int8_t>(gain);
    }
    if (const auto* eq = dac->find("eq")) {
      if (!eq->is_object()) {
        return false;
      }
      if (const auto* v = eq->find("mode")) {
        auto mode = eqModeFromName(v->as<std::string>());
        if (!mode) {
          return false;
        }
        newEqMode = *mode;
      }
      if (const auto* v = eq->find("gainL")) {
        if (!v->is_array() || v->get_array().size() != 15) {
          return false;
        }
        std::array<int8_t, 15> gains{};
        size_t i = 0;
        for (const auto& item : v->get_array()) {
          auto g = item.as<int32_t>();
          if (!inRange(g, -15, 15)) {
            return false;
          }
          gains[i++] = static_cast<int8_t>(g);
        }
        newEqGainL = gains;
      }
      if (const auto* v = eq->find("gainR")) {
        if (!v->is_array() || v->get_array().size() != 15) {
          return false;
        }
        std::array<int8_t, 15> gains{};
        size_t i = 0;
        for (const auto& item : v->get_array()) {
          auto g = item.as<int32_t>();
          if (!inRange(g, -15, 15)) {
            return false;
          }
          gains[i++] = static_cast<int8_t>(g);
        }
        newEqGainR = gains;
      }
      if (const auto* v = eq->find("profileL")) {
        auto profile = eqProfileFromName(v->as<std::string>());
        if (!profile) {
          return false;
        }
        newEqProfileL = *profile;
      }
      if (const auto* v = eq->find("profileR")) {
        auto profile = eqProfileFromName(v->as<std::string>());
        if (!profile) {
          return false;
        }
        newEqProfileR = *profile;
      }
    }
  } catch (const std::exception&) {
    return false;
  }

  if (newAnalogGain) {
    setAnalogGain(*newAnalogGain);
  }
  if (newDacMode) {
    setDacMode(*newDacMode);
  }
  if (newModMode) {
    setModulation(*newModMode, *newSwFreq, *newBdFreq);
  }
  if (newMixerMode) {
    setMixerMode(*newMixerMode);
  }
  if (newChannelGainL || newChannelGainR) {
    setChannelGain(newChannelGainL.value_or(channelGainLeft()),
                   newChannelGainR.value_or(channelGainRight()));
  }
  if (newEqMode) {
    setEqMode(*newEqMode);
  }
  if (newEqGainL || newEqGainR) {
    setEqGain(newEqGainL.value_or(eqGainLeft()),
              newEqGainR.value_or(eqGainRight()));
  }
  if (newEqProfileL || newEqProfileR) {
    setEqProfile(newEqProfileL.value_or(eqProfileLeft()),
                newEqProfileR.value_or(eqProfileRight()));
  }
  return true;
}

}  // namespace snapclient
