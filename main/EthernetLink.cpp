#include "EthernetLink.h"

#include "sdkconfig.h"

#if CONFIG_ETHERNET_INTERNAL_SUPPORT || CONFIG_ETHERNET_SPI_SUPPORT

#include <cstdio>
#include <cstdlib>

#include "esp_eth.h"
#include "esp_eth_netif_glue.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "ethernet_init.h"

namespace snapclient {

namespace {

const char* TAG = "eth";

// Above WIFI_STA's default of 100, so a wired link holds the default route
// while it is up and WiFi takes it back when it isn't.
constexpr int kRoutePrio = 150;

void onEthEvent(void* /*arg*/, esp_event_base_t /*base*/, int32_t eventId,
                void* eventData) {
  switch (eventId) {
    case ETHERNET_EVENT_CONNECTED: {
      auto handle = *static_cast<esp_eth_handle_t*>(eventData);
      uint8_t mac[6] = {};
      esp_eth_ioctl(handle, ETH_CMD_G_MAC_ADDR, mac);
      ESP_LOGI(TAG, "link up, %02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1],
               mac[2], mac[3], mac[4], mac[5]);
      break;
    }
    case ETHERNET_EVENT_DISCONNECTED:
      ESP_LOGW(TAG, "link down");
      break;
    default:
      break;
  }
}

void onGotIp(void* /*arg*/, esp_event_base_t /*base*/, int32_t /*eventId*/,
             void* eventData) {
  const auto* event = static_cast<ip_event_got_ip_t*>(eventData);
  ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&event->ip_info.ip));
}

bool attachAndStart(esp_eth_handle_t handle, uint8_t index) {
  esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
  char ifKey[8];
  char ifDesc[8];
  std::snprintf(ifKey, sizeof(ifKey), "ETH_%u", index);
  std::snprintf(ifDesc, sizeof(ifDesc), "eth%u", index);
  // esp_netif_new() strdups both, so these buffers need not outlive it.
  base.if_key = ifKey;
  base.if_desc = ifDesc;
  base.route_prio = kRoutePrio - index;

  esp_netif_config_t config = {.base = &base,
                               .driver = nullptr,
                               .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH};
  esp_netif_t* netif = esp_netif_new(&config);
  if (netif == nullptr) {
    ESP_LOGE(TAG, "esp_netif_new failed for %s", ifDesc);
    return false;
  }

  esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(handle);
  if (glue == nullptr) {
    ESP_LOGE(TAG, "esp_eth_new_netif_glue failed for %s", ifDesc);
    esp_netif_destroy(netif);
    return false;
  }

  if (esp_err_t err = esp_netif_attach(netif, glue); err != ESP_OK) {
    ESP_LOGE(TAG, "esp_netif_attach failed for %s: %s", ifDesc,
             esp_err_to_name(err));
    esp_eth_del_netif_glue(glue);
    esp_netif_destroy(netif);
    return false;
  }

  if (esp_err_t err = esp_eth_start(handle); err != ESP_OK) {
    ESP_LOGE(TAG, "esp_eth_start failed for %s: %s", ifDesc,
             esp_err_to_name(err));
    return false;
  }

  return true;
}

}  // namespace

bool ethernetStart() {
  esp_eth_handle_t* handles = nullptr;
  uint8_t count = 0;
  if (esp_err_t err = ethernet_init_all(&handles, &count); err != ESP_OK) {
    ESP_LOGE(TAG, "ethernet_init_all failed: %s", esp_err_to_name(err));
    return false;
  }

  esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &onEthEvent, nullptr);
  esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &onGotIp, nullptr);

  uint8_t started = 0;
  for (uint8_t i = 0; i < count; i++) {
    if (attachAndStart(handles[i], i)) {
      started++;
    }
  }

  // Only the array is ours - ethernet_deinit_all() works off the
  // component's own globals, and nothing here ever takes the link down.
  free(handles);

  ESP_LOGI(TAG, "%u of %u interface(s) started", started, count);
  return started > 0;
}

}  // namespace snapclient

#else

namespace snapclient {

bool ethernetStart() {
  return false;
}

}  // namespace snapclient

#endif
