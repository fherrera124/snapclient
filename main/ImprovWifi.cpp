#include "ImprovWifi.h"

#include <cstdio>
#include <cstring>

#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace snapclient {

namespace {
constexpr char kMagic[6] = {'I', 'M', 'P', 'R', 'O', 'V'};
constexpr const char* kLogTag = "ImprovWifi";
}  // namespace

std::atomic<bool> ImprovWifi::provisioningInProgress_{false};

bool ImprovWifi::isProvisioning() {
  return provisioningInProgress_;
}

ImprovWifi::ImprovWifi()
    : bell::Task("improv_wifi", 8 * 1024, /*espPriority=*/4,
                bell::TaskCore::CoreAny, /*espStackOnPsram=*/false) {
#if CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
  uart_driver_install(static_cast<uart_port_t>(CONFIG_ESP_CONSOLE_UART_NUM),
                      1024, 0, 0, nullptr, 0);
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED
  usb_serial_jtag_driver_config_t cfg =
      USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
  usb_serial_jtag_driver_install(&cfg);
#endif
  esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &onWifiEvent, this);
  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &onWifiEvent, this);
  startTask();
}

ImprovWifi::~ImprovWifi() {
  stopTask();
  esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &onWifiEvent);
  esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &onWifiEvent);
#if CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
  uart_driver_delete(static_cast<uart_port_t>(CONFIG_ESP_CONSOLE_UART_NUM));
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED
  usb_serial_jtag_driver_uninstall();
#endif
}

void ImprovWifi::onWifiEvent(void* arg, esp_event_base_t base, int32_t id,
                             void* data) {
  auto* self = static_cast<ImprovWifi*>(arg);
  if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    self->connected_ = true;
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    self->connected_ = false;
    // Driver connect-attempt tracing is compiled out at this log level;
    // this reason code is the only visible signal for why it failed.
    auto* disconnected = static_cast<wifi_event_sta_disconnected_t*>(data);
    ESP_LOGW(kLogTag, "STA disconnected, reason=%d", disconnected->reason);
  }
}

void ImprovWifi::taskLoop() {
  uint8_t buf[64];
  int len = 0;
#if CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
  len = uart_read_bytes(static_cast<uart_port_t>(CONFIG_ESP_CONSOLE_UART_NUM),
                        buf, sizeof(buf), pdMS_TO_TICKS(100));
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED
  len = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(100));
#endif
  for (int i = 0; i < len; i++) {
    handleByte(buf[i]);
  }
}

void ImprovWifi::handleByte(uint8_t b) {
  if (framePos_ < 6) {
    if (b != static_cast<uint8_t>(kMagic[framePos_])) {
      framePos_ = 0;
      return;
    }
    frame_[framePos_++] = b;
    return;
  }
  if (framePos_ == 6) {
    if (b != kProtocolVersion) {
      framePos_ = 0;
      return;
    }
    frame_[framePos_++] = b;
    return;
  }
  if (framePos_ == 7 || framePos_ == 8) {
    frame_[framePos_++] = b;
    return;
  }

  const uint8_t dataLen = frame_[8];
  const size_t payloadEnd = 9 + dataLen;
  if (framePos_ < payloadEnd) {
    frame_[framePos_++] = b;
    return;
  }

  // framePos_ == payloadEnd: this byte is the checksum.
  uint8_t checksum = 0;
  for (size_t i = 0; i < payloadEnd; i++) {
    checksum += frame_[i];
  }
  framePos_ = 0;
  if (checksum != b) {
    sendError(ErrorCode::InvalidRpc);
    return;
  }
  if (frame_[7] == static_cast<uint8_t>(FrameType::Rpc)) {
    onFrameComplete(&frame_[9], dataLen);
  }
}

void ImprovWifi::onFrameComplete(const uint8_t* payload, uint8_t len) {
  if (len < 2) {
    sendError(ErrorCode::InvalidRpc);
    return;
  }
  auto command = static_cast<Command>(payload[0]);
  uint8_t innerLen = payload[1];
  if (innerLen != len - 2) {
    sendError(ErrorCode::InvalidRpc);
    return;
  }
  handleCommand(command, payload + 2, innerLen);
}

void ImprovWifi::handleCommand(Command cmd, const uint8_t* data,
                               uint8_t len) {
  switch (cmd) {
    case Command::GetCurrentState: {
      if (isConnected()) {
        sendState(State::Provisioned);
        sendRpcResponse(cmd, {deviceUrl()});
      } else {
        sendState(State::Authorized);
      }
      break;
    }

    case Command::WifiSettings: {
      if (len < 1) {
        sendError(ErrorCode::InvalidRpc);
        break;
      }
      uint8_t ssidLen = data[0];
      if (1u + ssidLen + 1u > len) {
        sendError(ErrorCode::InvalidRpc);
        break;
      }
      std::string ssid(reinterpret_cast<const char*>(data + 1), ssidLen);
      uint8_t passLen = data[1 + ssidLen];
      size_t passStart = 1u + ssidLen + 1u;
      if (passStart + passLen > len) {
        sendError(ErrorCode::InvalidRpc);
        break;
      }
      std::string password(reinterpret_cast<const char*>(data + passStart),
                           passLen);
      if (ssid.empty()) {
        sendError(ErrorCode::InvalidRpc);
        break;
      }

      sendState(State::Provisioning);
      if (connectWifi(ssid, password)) {
        sendError(ErrorCode::None);
        sendState(State::Provisioned);
        sendRpcResponse(cmd, {deviceUrl()});
        if (onProvisioned) {
          onProvisioned();
        }
      } else {
        // Stopped means "provisioning unavailable" in the Improv spec, not
        // "attempt failed" - stay Authorized so the client can retry.
        sendError(ErrorCode::UnableToConnect);
        sendState(State::Authorized);
      }
      break;
    }

    case Command::GetDeviceInfo:
      sendRpcResponse(cmd, deviceInfoFields());
      break;

    case Command::GetWifiNetworks:
      sendWifiNetworks();
      break;

    default:
      sendError(ErrorCode::UnknownRpc);
      break;
  }
}

bool ImprovWifi::connectWifi(const std::string& ssid,
                             const std::string& password) {
  wifi_config_t config{};
  if (ssid.size() >= sizeof(config.sta.ssid) ||
      password.size() >= sizeof(config.sta.password)) {
    return false;
  }

  // Held for the whole call so main.cpp's disconnect handler steps aside
  // instead of racing the sequence below.
  provisioningInProgress_ = true;
  struct ScopeGuard {
    ~ScopeGuard() { provisioningInProgress_ = false; }
  } guard;

  // esp_wifi_disconnect() only posts the disconnect and returns immediately,
  // so set_config() can still see ESP_ERR_WIFI_STATE ("still connecting")
  // for a few ms after - retry instead of failing on the first attempt.
  connected_ = false;
  esp_wifi_disconnect();

  // Not seeded from esp_wifi_get_config(): it returns threshold.authmode
  // and bssid still pinned to the previously-associated AP, which would
  // silently restrict scanning to a network that's no longer wanted.
  std::memcpy(config.sta.ssid, ssid.data(), ssid.size());
  std::memcpy(config.sta.password, password.data(), password.size());

  esp_err_t err = ESP_FAIL;
  for (int attempt = 0; attempt < 10; attempt++) {
    err = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (err != ESP_ERR_WIFI_STATE) {
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  if (err != ESP_OK) {
    ESP_LOGW(kLogTag, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
    return false;
  }

  err = esp_wifi_connect();
  if (err != ESP_OK) {
    ESP_LOGW(kLogTag, "esp_wifi_connect failed: %s", esp_err_to_name(err));
    return false;
  }

  for (int attempt = 0; attempt < 20; attempt++) {
    if (connected_) {
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  ESP_LOGW(kLogTag, "timed out waiting to connect to \"%s\"", ssid.c_str());
  return false;
}

bool ImprovWifi::isConnected() {
  return connected_;
}

std::string ImprovWifi::deviceUrl() {
  esp_netif_ip_info_t ipInfo{};
  esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (netif != nullptr) {
    esp_netif_get_ip_info(netif, &ipInfo);
  }
  char buf[24];
  snprintf(buf, sizeof(buf), "http://" IPSTR, IP2STR(&ipInfo.ip));
  return std::string(buf);
}

std::vector<std::string> ImprovWifi::deviceInfoFields() {
#if CONFIG_IDF_TARGET_ESP32S3
  const char* chipFamily = "ESP32-S3";
#elif CONFIG_IDF_TARGET_ESP32S2
  const char* chipFamily = "ESP32-S2";
#elif CONFIG_IDF_TARGET_ESP32C3
  const char* chipFamily = "ESP32-C3";
#else
  const char* chipFamily = "ESP32";
#endif
  return {"snapclient", "0.1.0", chipFamily, "snapclient"};
}

void ImprovWifi::sendWifiNetworks() {
  wifi_scan_config_t scanConfig{};
  esp_wifi_scan_start(&scanConfig, true);

  uint16_t count = 0;
  esp_wifi_scan_get_ap_num(&count);
  if (count > 20) {
    count = 20;
  }
  std::vector<wifi_ap_record_t> records(count);
  esp_wifi_scan_get_ap_records(&count, records.data());

  for (uint16_t i = 0; i < count; i++) {
    sendRpcResponse(
        Command::GetWifiNetworks,
        {reinterpret_cast<const char*>(records[i].ssid),
         std::to_string(records[i].rssi),
         records[i].authmode == WIFI_AUTH_OPEN ? "NO" : "YES"});
  }
  sendRpcResponse(Command::GetWifiNetworks, {});
}

void ImprovWifi::sendState(State state) {
  writeFrame(FrameType::CurrentState, {static_cast<uint8_t>(state)});
}

void ImprovWifi::sendError(ErrorCode error) {
  writeFrame(FrameType::ErrorState, {static_cast<uint8_t>(error)});
}

void ImprovWifi::sendRpcResponse(Command cmd,
                                 const std::vector<std::string>& fields) {
  std::vector<uint8_t> payload;
  payload.push_back(static_cast<uint8_t>(cmd));
  size_t lenPos = payload.size();
  payload.push_back(0);
  size_t dataStart = payload.size();
  for (const auto& field : fields) {
    payload.push_back(static_cast<uint8_t>(field.size()));
    payload.insert(payload.end(), field.begin(), field.end());
  }
  payload[lenPos] = static_cast<uint8_t>(payload.size() - dataStart);
  writeFrame(FrameType::RpcResponse, payload);
}

void ImprovWifi::writeFrame(FrameType type,
                            const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> frame(kMagic, kMagic + 6);
  frame.push_back(kProtocolVersion);
  frame.push_back(static_cast<uint8_t>(type));
  frame.push_back(static_cast<uint8_t>(payload.size()));
  frame.insert(frame.end(), payload.begin(), payload.end());

  uint8_t checksum = 0;
  for (uint8_t b : frame) {
    checksum += b;
  }
  frame.push_back(checksum);

  writeBytes(frame.data(), frame.size());
}

void ImprovWifi::writeBytes(const uint8_t* data, size_t len) {
  // ESP_LOGx()/BELL_LOG() share this UART via stdout; this lock keeps a
  // concurrent log line from splicing into the frame mid-write.
  flockfile(stdout);
#if CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
  uart_write_bytes(static_cast<uart_port_t>(CONFIG_ESP_CONSOLE_UART_NUM),
                   reinterpret_cast<const char*>(data), len);
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED
  usb_serial_jtag_write_bytes(data, len, portMAX_DELAY);
#endif
  funlockfile(stdout);
}

}  // namespace snapclient
