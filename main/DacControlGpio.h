#pragma once

#include "DacControl.h"
#include "driver/gpio.h"

namespace snapclient {

// Passive DACs (PCM5102A, MAX98357A, ...): nothing to bring up, mute at
// most through one GPIO, driven low to mute.
class DacControlGpio : public DacControl {
 public:
  // GPIO_NUM_NC disables mute handling entirely (e.g. XSMT tied via an
  // onboard pull-up, not software-controlled).
  explicit DacControlGpio(gpio_num_t mutePin);

  esp_err_t start(i2s_chan_handle_t tx, uint32_t sampleRate) override;
  void setMuted(bool muted) override;

 private:
  gpio_num_t mutePin_;
};

}  // namespace snapclient
