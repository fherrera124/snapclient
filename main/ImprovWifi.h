#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <bell/utils/Task.h>

#include "esp_event.h"

namespace snapclient {

// Improv Serial (improv-wifi.com) WiFi provisioning over whichever
// interface ESP-IDF's console Kconfig selects (UART or USB-Serial-JTAG).
// Listens whether or not credentials are already stored - reprovisioning
// doesn't need a factory reset - until CONFIG_SNAPCLIENT_IMPROV_TIMEOUT_S
// elapses, if that is set.
class ImprovWifi : public bell::Task {
 public:
  ImprovWifi();
  ~ImprovWifi() override;

  // Fired after a WIFI_SETTINGS command successfully connects.
  std::function<void()> onProvisioned;

  // True while connectWifi() is running - main.cpp's disconnect handler
  // checks this to avoid racing connectWifi()'s own reconnect sequence.
  static bool isProvisioning();

 protected:
  void taskLoop() override;

 private:
  enum class State : uint8_t {
    AwaitingAuthorization = 0x01,
    Authorized = 0x02,
    Provisioning = 0x03,
    Provisioned = 0x04,
  };
  enum class ErrorCode : uint8_t {
    None = 0x00,
    InvalidRpc = 0x01,
    UnknownRpc = 0x02,
    UnableToConnect = 0x03,
    NotAuthorized = 0x04,
    Unknown = 0xFF,
  };
  enum class Command : uint8_t {
    Unknown = 0x00,
    WifiSettings = 0x01,
    GetCurrentState = 0x02,
    GetDeviceInfo = 0x03,
    GetWifiNetworks = 0x04,
  };
  enum class FrameType : uint8_t {
    CurrentState = 0x01,
    ErrorState = 0x02,
    Rpc = 0x03,
    RpcResponse = 0x04,
  };

  static constexpr uint8_t kProtocolVersion = 1;
  static constexpr size_t kMaxPayloadLen = 255;

  size_t framePos_ = 0;
  std::array<uint8_t, 9 + kMaxPayloadLen> frame_{};

  // esp_timer_get_time() value past which taskLoop() stops the task. 0
  // means never.
  int64_t deadlineUs_ = 0;

  void handleByte(uint8_t b);
  void onFrameComplete(const uint8_t* payload, uint8_t len);
  void handleCommand(Command cmd, const uint8_t* data, uint8_t len);

  void sendState(State state);
  void sendError(ErrorCode error);
  void sendRpcResponse(Command cmd, const std::vector<std::string>& fields);
  void writeFrame(FrameType type, const std::vector<uint8_t>& payload);
  void writeBytes(const uint8_t* data, size_t len);

  bool connectWifi(const std::string& ssid, const std::string& password);
  bool isConnected();
  std::string deviceUrl();
  std::vector<std::string> deviceInfoFields();
  void sendWifiNetworks();

  // esp_wifi_sta_get_ap_info() logs a warning every time it's asked about
  // a station that isn't connected yet, so connection state is tracked
  // from WIFI_EVENT/IP_EVENT here.
  std::atomic<bool> connected_{false};
  static void onWifiEvent(void* arg, esp_event_base_t base, int32_t id,
                          void* data);

  static std::atomic<bool> provisioningInProgress_;
};

}  // namespace snapclient
