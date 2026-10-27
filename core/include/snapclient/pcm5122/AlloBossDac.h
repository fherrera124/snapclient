#pragma once

#include <cstdint>

#include "snapclient/pcm5122/Pcm5122Driver.h"

namespace snapclient {

// Allo Boss: a PCM5122 driving I2S from one of two onboard oscillators,
// switched through its own GPIOs. Follows Linux's allo-boss-dac.c.
class AlloBossDac {
 public:
  explicit AlloBossDac(Pcm5122Driver& dac);

  bool reset();

  // Leaves the chip in standby on any failure, so it never drives clocks it
  // was not configured for. False also when the board gives no SCK, i.e. it
  // cannot be the master.
  bool start(uint32_t sampleRate);

  bool setMuted(bool muted);
  bool setVolumeCode(uint8_t code);

  // Both oscillators off; BCK and LRCK come from the host.
  bool startSlave();

 private:
  Pcm5122Driver& dac_;
};

}  // namespace snapclient
