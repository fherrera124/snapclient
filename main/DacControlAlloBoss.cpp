#include "DacControlAlloBoss.h"

#include "bell/Logger.h"
#include "driver/gpio.h"
#include "sdkconfig.h"

namespace snapclient {

DacControlAlloBoss::DacControlAlloBoss(EspI2cBus& bus, uint8_t deviceAddr,
                                       gpio_num_t mutePin)
    : chip_(bus, deviceAddr), boss_(chip_), muteLine_(mutePin) {
  if (!boss_.reset()) {
    BELL_LOG(error, LOG_TAG, "no answer from the PCM5122 at 0x{:02x}",
             deviceAddr);
    bus.logScan();
  }
}

esp_err_t DacControlAlloBoss::start(i2s_chan_handle_t /*tx*/,
                                    [[maybe_unused]] uint32_t sampleRate) {
#if CONFIG_SNAPCLIENT_ALLO_BOSS_SLAVE
  // Softer edges, so the clock leads ring less.
  gpio_set_drive_capability(
      static_cast<gpio_num_t>(CONFIG_SNAPCLIENT_I2S_BCLK_GPIO),
      GPIO_DRIVE_CAP_1);
  gpio_set_drive_capability(
      static_cast<gpio_num_t>(CONFIG_SNAPCLIENT_I2S_WS_GPIO),
      GPIO_DRIVE_CAP_1);
  if (!boss_.startSlave()) {
    BELL_LOG(error, LOG_TAG, "could not start as I2S slave");
    return ESP_FAIL;
  }
  return ESP_OK;
#else
  if (!boss_.start(sampleRate)) {
    BELL_LOG(error, LOG_TAG,
             "could not start as I2S master at {} Hz (no I2C, no SCK, or "
             "an unsupported rate)",
             sampleRate);
    return ESP_FAIL;
  }
  return ESP_OK;
#endif
}

void DacControlAlloBoss::setMuted(bool muted) {
  muteLine_.setMuted(muted);
  boss_.setMuted(muted);
}

}  // namespace snapclient
