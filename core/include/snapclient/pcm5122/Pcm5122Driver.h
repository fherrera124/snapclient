#pragma once

#include <cstdint>
#include <optional>

#include "snapclient/I2cBus.h"

namespace snapclient {

// Clock dividers for the PCM5122 as I2S master, fed from an SCK with its PLL
// off. Each divider is the real ratio, not the register's ratio minus one.
struct Pcm5122MasterDividers {
  uint8_t dsp = 1;
  uint8_t dac = 1;
  uint8_t ncp = 1;
  uint8_t osr = 1;
  uint8_t bck = 1;
  uint8_t lrck = 1;
  uint16_t idac = 0;
  uint8_t fsSpeed = 0;  // 0 = up to 48k, 1 = 96k, 2 = 192k, 3 = 384k
};

// Mirrors pcm512x_set_dividers() in Linux for the no-PLL case. nullopt when
// sckHz does not divide down exactly to sampleRate * bckPerFrame.
std::optional<Pcm5122MasterDividers> pcm5122MasterDividers(
    uint32_t sckHz, uint32_t sampleRate, uint32_t bckPerFrame);

// Digital volume register value for a linear gain: 0.5 dB steps from
// 0x30 (0 dB) down, 0xFF (mute) at or below -103.5 dB. Above 1 is 0 dB.
uint8_t pcm5122VolumeCode(float gain);

// Page 0 registers only; nothing here needs another page.
class Pcm5122Driver {
 public:
  explicit Pcm5122Driver(I2cBus& bus, uint8_t deviceAddr = 0x4D);

  bool reset();
  bool setStandby(bool standby);
  bool setMuted(bool muted);
  // Both channels; the chip ramps to it on its own.
  bool setVolumeCode(uint8_t code);

  // Hands the GPIOs in mask (bit n-1 = GPIOn) to setGpioLevels().
  bool enableRegisterGpios(uint8_t mask);
  // Same bit layout; writes every GPIO at once.
  bool setGpioLevels(uint8_t levels);

  bool sckPresent(bool& present);

  // I2S format, 16-bit words, BCK and LRCK driven by the chip.
  bool configureMaster(const Pcm5122MasterDividers& dividers);

  // I2S format, 16-bit words, BCK and LRCK in, PLL fed from BCK.
  bool configureSlave();

 private:
  I2cBus& bus_;
  uint8_t deviceAddr_;

  bool writeByte(uint8_t reg, uint8_t value);
  bool readByte(uint8_t reg, uint8_t& value);
};

}  // namespace snapclient
