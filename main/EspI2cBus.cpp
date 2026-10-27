#include "EspI2cBus.h"

#include "bell/Logger.h"
#include "esp_rom_sys.h"

namespace snapclient {

namespace {
// Slow enough for the internal pull-ups alone on short wires; a DAC's
// bring-up is a few dozen register writes, so speed buys nothing.
constexpr uint32_t kSclSpeedHz = 50'000;
constexpr int kTimeoutMs = 50;
}  // namespace

EspI2cBus::EspI2cBus(gpio_num_t sda, gpio_num_t scl) : sda_(sda), scl_(scl) {
  // A reset mid-transfer can leave the slave holding SDA low: clock it out
  // with up to 9 SCL pulses, then send a STOP.
  gpio_set_direction(sda, GPIO_MODE_INPUT_OUTPUT_OD);
  gpio_set_direction(scl, GPIO_MODE_INPUT_OUTPUT_OD);
  gpio_set_pull_mode(sda, GPIO_PULLUP_ONLY);
  gpio_set_pull_mode(scl, GPIO_PULLUP_ONLY);
  gpio_set_level(sda, 1);
  gpio_set_level(scl, 1);
  esp_rom_delay_us(20);
  if (gpio_get_level(sda) == 0) {
    int pulses = 0;
    while (pulses < 9 && gpio_get_level(sda) == 0) {
      gpio_set_level(scl, 0);
      esp_rom_delay_us(20);
      gpio_set_level(scl, 1);
      esp_rom_delay_us(20);
      pulses++;
    }
    gpio_set_level(sda, 0);
    esp_rom_delay_us(20);
    gpio_set_level(sda, 1);
    esp_rom_delay_us(20);
    BELL_LOG(warn, LOG_TAG, "SDA was held low; {} clock pulses, now SDA={}",
             pulses, gpio_get_level(sda));
  }
  i2c_master_bus_config_t config = {};
  config.i2c_port = -1;
  config.sda_io_num = sda;
  config.scl_io_num = scl;
  config.clk_source = I2C_CLK_SRC_DEFAULT;
  config.glitch_ignore_cnt = 7;
  config.flags.enable_internal_pullup = true;
  esp_err_t err = i2c_new_master_bus(&config, &bus_);
  if (err != ESP_OK) {
    BELL_LOG(error, LOG_TAG, "i2c_new_master_bus failed: {}",
             static_cast<int>(err));
    bus_ = nullptr;
  }
}

EspI2cBus::~EspI2cBus() {
  for (auto& [addr, dev] : devices_) {
    i2c_master_bus_rm_device(dev);
  }
  if (bus_ != nullptr) {
    i2c_del_master_bus(bus_);
  }
}

i2c_master_dev_handle_t EspI2cBus::device(uint8_t deviceAddr) {
  if (bus_ == nullptr) {
    return nullptr;
  }
  auto it = devices_.find(deviceAddr);
  if (it != devices_.end()) {
    return it->second;
  }
  i2c_device_config_t config = {};
  config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  config.device_address = deviceAddr;
  config.scl_speed_hz = kSclSpeedHz;
  i2c_master_dev_handle_t dev = nullptr;
  esp_err_t err = i2c_master_bus_add_device(bus_, &config, &dev);
  if (err != ESP_OK) {
    BELL_LOG(error, LOG_TAG, "adding device 0x{:02x} failed: {}", deviceAddr,
             static_cast<int>(err));
    return nullptr;
  }
  devices_[deviceAddr] = dev;
  return dev;
}

bool EspI2cBus::write(uint8_t deviceAddr, const uint8_t* data, size_t len) {
  i2c_master_dev_handle_t dev = device(deviceAddr);
  return dev != nullptr &&
         i2c_master_transmit(dev, data, len, kTimeoutMs) == ESP_OK;
}

bool EspI2cBus::writeThenRead(uint8_t deviceAddr, const uint8_t* writeData,
                              size_t writeLen, uint8_t* readBuf,
                              size_t readLen) {
  i2c_master_dev_handle_t dev = device(deviceAddr);
  return dev != nullptr &&
         i2c_master_transmit_receive(dev, writeData, writeLen, readBuf,
                                     readLen, kTimeoutMs) == ESP_OK;
}

void EspI2cBus::logScan() {
  BELL_LOG(info, LOG_TAG, "idle levels: SDA(GPIO{})={} SCL(GPIO{})={}",
           static_cast<int>(sda_), gpio_get_level(sda_),
           static_cast<int>(scl_), gpio_get_level(scl_));
  if (bus_ == nullptr) {
    return;
  }
  int found = 0;
  for (uint16_t addr = 0x08; addr < 0x78; addr++) {
    if (i2c_master_probe(bus_, addr, kTimeoutMs) == ESP_OK) {
      BELL_LOG(info, LOG_TAG, "device answers at 0x{:02x}", addr);
      found++;
    }
  }
  BELL_LOG(info, LOG_TAG, "scan done: {} device(s)", found);
}

}  // namespace snapclient
