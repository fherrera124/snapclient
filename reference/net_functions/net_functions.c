/*
   Network related functions
*/

#include "net_functions.h"

#include "esp_log.h"
#include "mdns.h"

static const char *TAG = "NETF";

void net_mdns_register(const char *clientname) {
  ESP_LOGI(TAG, "Setup mdns");
  ESP_ERROR_CHECK(mdns_init());
  ESP_ERROR_CHECK(mdns_hostname_set(clientname));
  ESP_ERROR_CHECK(mdns_instance_name_set("ESP32 SNAPcast client OTA"));
  ESP_ERROR_CHECK(mdns_service_add(NULL, "_http", "_tcp", 8032, NULL, 0));
}
