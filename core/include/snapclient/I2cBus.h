#pragma once

#include <cstddef>
#include <cstdint>

namespace snapclient {

// bell has no I2C support - this is what lets a driver's register-write
// logic be recorded and asserted against on a host with no real bus.
class I2cBus {
 public:
  virtual ~I2cBus() = default;

  virtual bool write(uint8_t deviceAddr, const uint8_t* data, size_t len) = 0;

  virtual bool writeThenRead(uint8_t deviceAddr, const uint8_t* writeData,
                             size_t writeLen, uint8_t* readBuf,
                             size_t readLen) = 0;
};

}  // namespace snapclient
