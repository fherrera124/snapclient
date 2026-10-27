#pragma once

#include <cstddef>
#include <cstdint>
#include <map>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "snapclient/I2cBus.h"

namespace snapclient {

// I2cBus over ESP-IDF's i2c_master driver, at 50 kHz. Each address is
// added as a device the first time it is used.
class EspI2cBus : public I2cBus {
 public:
  EspI2cBus(gpio_num_t sda, gpio_num_t scl);
  ~EspI2cBus() override;

  EspI2cBus(const EspI2cBus&) = delete;
  EspI2cBus& operator=(const EspI2cBus&) = delete;

  bool write(uint8_t deviceAddr, const uint8_t* data, size_t len) override;
  bool writeThenRead(uint8_t deviceAddr, const uint8_t* writeData,
                     size_t writeLen, uint8_t* readBuf,
                     size_t readLen) override;

  // Logs the idle level of both lines and every address that acks.
  void logScan();

 private:
  const char* LOG_TAG = "EspI2cBus";

  gpio_num_t sda_;
  gpio_num_t scl_;
  i2c_master_bus_handle_t bus_ = nullptr;
  std::map<uint8_t, i2c_master_dev_handle_t> devices_;

  i2c_master_dev_handle_t device(uint8_t deviceAddr);
};

}  // namespace snapclient
