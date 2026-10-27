#include "DacControlGpio.h"

namespace snapclient {

DacControlGpio::DacControlGpio(gpio_num_t mutePin) : mutePin_(mutePin) {
  if (mutePin_ != GPIO_NUM_NC) {
    gpio_config_t muteConf = {
        .pin_bit_mask = 1ULL << mutePin_,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&muteConf);
  }
}

esp_err_t DacControlGpio::start(i2s_chan_handle_t /*tx*/,
                                uint32_t /*sampleRate*/) {
  return ESP_OK;
}

void DacControlGpio::setMuted(bool muted) {
  if (mutePin_ == GPIO_NUM_NC) {
    return;
  }
  gpio_set_level(mutePin_, muted ? 0 : 1);
}

}  // namespace snapclient
