#pragma once

#include "DacControl.h"
#include "DacControlGpio.h"
#include "EspI2cBus.h"
#include "snapclient/pcm5122/AlloBossDac.h"
#include "snapclient/pcm5122/Pcm5122Driver.h"

namespace snapclient {

// Allo Boss, with the PCM5122 doing volume. Mutes both the PCM5122 and, if
// wired, the board's own mute line (the Pi header's GPIO6, active low).
class DacControlAlloBoss : public DacControl {
 public:
  // bus must outlive this.
  DacControlAlloBoss(EspI2cBus& bus, uint8_t deviceAddr, gpio_num_t mutePin);

  esp_err_t start(i2s_chan_handle_t tx, uint32_t sampleRate) override;
  void setMuted(bool muted) override;
  bool setVolume(float gain) override;

 private:
  const char* LOG_TAG = "DacControlAlloBoss";

  Pcm5122Driver chip_;
  AlloBossDac boss_;
  DacControlGpio muteLine_;
  // Last code written; -1 forces the next write.
  int volumeCode_ = -1;
};

}  // namespace snapclient
