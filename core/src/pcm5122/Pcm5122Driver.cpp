#include "snapclient/pcm5122/Pcm5122Driver.h"

namespace snapclient {

namespace {

constexpr uint8_t kRegPage = 0;
constexpr uint8_t kRegReset = 1;
constexpr uint8_t kRegPower = 2;
constexpr uint8_t kRegMute = 3;
constexpr uint8_t kRegPllEn = 4;
constexpr uint8_t kRegGpioEn = 8;
constexpr uint8_t kRegBckLrckCfg = 9;
constexpr uint8_t kRegMasterMode = 12;
constexpr uint8_t kRegPllRef = 13;
constexpr uint8_t kRegDacRef = 14;
constexpr uint8_t kRegSynchronize = 19;
constexpr uint8_t kRegDspClkDiv = 27;
constexpr uint8_t kRegDacClkDiv = 28;
constexpr uint8_t kRegNcpClkDiv = 29;
constexpr uint8_t kRegOsrClkDiv = 30;
constexpr uint8_t kRegMasterClkDiv1 = 32;
constexpr uint8_t kRegMasterClkDiv2 = 33;
constexpr uint8_t kRegFsSpeedMode = 34;
constexpr uint8_t kRegIdac1 = 35;
constexpr uint8_t kRegIdac2 = 36;
constexpr uint8_t kRegErrorDetect = 37;
constexpr uint8_t kRegI2s1 = 40;
constexpr uint8_t kRegI2s2 = 41;
constexpr uint8_t kRegGpioOutput1 = 80;
constexpr uint8_t kRegGpioControl1 = 86;
constexpr uint8_t kRegRateDet4 = 94;

constexpr uint8_t kResetModulesAndRegisters = 0x11;
constexpr uint8_t kPowerStandby = 0x10;
constexpr uint8_t kMuteBoth = 0x11;
constexpr uint8_t kBckOutLrckOut = 0x11;
constexpr uint8_t kMasterDividersRunning = 0x03;
constexpr uint8_t kDacRefSck = 0x30;
constexpr uint8_t kPllRefBck = 0x10;
constexpr uint8_t kGpioOutputFromRegister = 0x02;
constexpr uint8_t kSckMissing = 0x40;
// IDFS|IDBK|IDSK|IDCH|DCAS|IPLK: no auto clock setup, ignore what it would
// have flagged. IDCM stays clear, so clock-missing detection still runs.
constexpr uint8_t kErrorDetectManualClocks = 0x7B;
// IDCH: no SCK in slave mode, so do not halt for its absence.
constexpr uint8_t kErrorDetectNoSck = 0x08;
constexpr uint8_t kI2s16Bit = 0x00;

constexpr uint32_t kDspMaxHz = 50'000'000;
constexpr uint32_t kDacMaxHz = 6'144'000;
constexpr uint32_t kNcpTargetHz = 1'536'000;
constexpr uint32_t kNcpMaxHz = 2'048'000;
constexpr uint32_t kMaxDivider = 128;

uint32_t divRoundClosest(uint32_t num, uint32_t den) {
  return (num + den / 2) / den;
}

}  // namespace

std::optional<Pcm5122MasterDividers> pcm5122MasterDividers(
    uint32_t sckHz, uint32_t sampleRate, uint32_t bckPerFrame) {
  if (sampleRate == 0 || bckPerFrame == 0 || bckPerFrame > kMaxDivider) {
    return std::nullopt;
  }
  const uint64_t bckHz = uint64_t{sampleRate} * bckPerFrame;
  if (sckHz % bckHz != 0 || sckHz / bckHz > kMaxDivider) {
    return std::nullopt;
  }

  Pcm5122MasterDividers d;
  d.bck = static_cast<uint8_t>(sckHz / bckHz);
  d.lrck = static_cast<uint8_t>(bckPerFrame);
  d.dsp = sckHz > kDspMaxHz ? 2 : 1;

  const uint32_t osrHz = 16 * sampleRate;
  const uint32_t sckMul = sckHz / osrHz;
  uint32_t dacMul = kDacMaxHz / osrHz;
  while (dacMul > 0 && sckMul % dacMul != 0) {
    dacMul--;
  }
  if (dacMul == 0) {
    return std::nullopt;
  }
  uint32_t dacHz = dacMul * osrHz;

  const uint32_t osrDiv = divRoundClosest(dacHz, osrHz);
  const uint32_t dacDiv = divRoundClosest(sckHz, dacHz);
  if (osrDiv > kMaxDivider || dacDiv > kMaxDivider) {
    return std::nullopt;
  }
  dacHz = sckHz / dacDiv;

  uint32_t ncpDiv = divRoundClosest(dacHz, kNcpTargetHz);
  if (ncpDiv == 0 || ncpDiv > kMaxDivider || dacHz / ncpDiv > kNcpMaxHz) {
    ncpDiv = (dacHz + kNcpMaxHz - 1) / kNcpMaxHz;
    if (ncpDiv > kMaxDivider) {
      return std::nullopt;
    }
  }

  d.osr = static_cast<uint8_t>(osrDiv);
  d.dac = static_cast<uint8_t>(dacDiv);
  d.ncp = static_cast<uint8_t>(ncpDiv);
  d.idac = static_cast<uint16_t>(sckHz / (d.dsp * sampleRate));
  d.fsSpeed = sampleRate <= 48000 ? 0 : sampleRate <= 96000 ? 1
              : sampleRate <= 192000 ? 2 : 3;
  return d;
}

Pcm5122Driver::Pcm5122Driver(I2cBus& bus, uint8_t deviceAddr)
    : bus_(bus), deviceAddr_(deviceAddr) {}

bool Pcm5122Driver::writeByte(uint8_t reg, uint8_t value) {
  uint8_t buf[2] = {reg, value};
  return bus_.write(deviceAddr_, buf, sizeof(buf));
}

bool Pcm5122Driver::readByte(uint8_t reg, uint8_t& value) {
  return bus_.writeThenRead(deviceAddr_, &reg, 1, &value, 1);
}

bool Pcm5122Driver::reset() {
  return writeByte(kRegPage, 0) &&
         writeByte(kRegReset, kResetModulesAndRegisters) &&
         writeByte(kRegReset, 0);
}

bool Pcm5122Driver::setStandby(bool standby) {
  return writeByte(kRegPower, standby ? kPowerStandby : 0);
}

bool Pcm5122Driver::setMuted(bool muted) {
  return writeByte(kRegMute, muted ? kMuteBoth : 0);
}

bool Pcm5122Driver::enableRegisterGpios(uint8_t mask) {
  if (!writeByte(kRegGpioEn, mask)) {
    return false;
  }
  for (uint8_t gpio = 0; gpio < 6; gpio++) {
    if ((mask & (1 << gpio)) &&
        !writeByte(kRegGpioOutput1 + gpio, kGpioOutputFromRegister)) {
      return false;
    }
  }
  return true;
}

bool Pcm5122Driver::setGpioLevels(uint8_t levels) {
  return writeByte(kRegGpioControl1, levels);
}

bool Pcm5122Driver::sckPresent(bool& present) {
  uint8_t value = 0;
  if (!readByte(kRegRateDet4, value)) {
    return false;
  }
  present = (value & kSckMissing) == 0;
  return true;
}

bool Pcm5122Driver::configureMaster(const Pcm5122MasterDividers& d) {
  return writeByte(kRegBckLrckCfg, kBckOutLrckOut) &&
         writeByte(kRegMasterMode, kMasterDividersRunning) &&
         writeByte(kRegErrorDetect, kErrorDetectManualClocks) &&
         writeByte(kRegPllEn, 0) &&
         writeByte(kRegI2s1, kI2s16Bit) &&
         writeByte(kRegI2s2, 0) &&
         writeByte(kRegDacRef, kDacRefSck) &&
         writeByte(kRegDspClkDiv, d.dsp - 1) &&
         writeByte(kRegDacClkDiv, d.dac - 1) &&
         writeByte(kRegNcpClkDiv, d.ncp - 1) &&
         writeByte(kRegOsrClkDiv, d.osr - 1) &&
         writeByte(kRegMasterClkDiv1, d.bck - 1) &&
         writeByte(kRegMasterClkDiv2, d.lrck - 1) &&
         writeByte(kRegIdac1, d.idac >> 8) &&
         writeByte(kRegIdac2, d.idac & 0xFF) &&
         writeByte(kRegFsSpeedMode, d.fsSpeed) &&
         // Halt and resume, so the new dividers take effect together.
         writeByte(kRegSynchronize, 1) &&
         writeByte(kRegSynchronize, 0);
}

bool Pcm5122Driver::configureSlave() {
  return writeByte(kRegBckLrckCfg, 0) &&
         writeByte(kRegMasterMode, 0) &&
         writeByte(kRegDacRef, 0) &&
         writeByte(kRegPllRef, kPllRefBck) &&
         writeByte(kRegErrorDetect, kErrorDetectNoSck) &&
         writeByte(kRegPllEn, 1) &&
         writeByte(kRegI2s1, kI2s16Bit) &&
         writeByte(kRegI2s2, 0);
}

}  // namespace snapclient
