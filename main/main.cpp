#include <bell/Logger.h>
#include <bell/mdns/Manager.h>
#include <bell/utils/Task.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <thread>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "AudioSinkI2S.h"
#include "EthernetLink.h"
#include "GptimerWaiter.h"
#include "ImprovWifi.h"
#include "NvsSettingsStore.h"
#include "snapclient/ControlServer.h"
#include "snapclient/ControlSettings.h"
#include "snapclient/Core.h"
#include "snapclient/PlaybackPipeline.h"
#include "snapclient/UdpLogBackend.h"

namespace {

const char* TAG = "snapclient";

bool netifHasIp(esp_netif_t* netif, void* /*ctx*/) {
  if (!esp_netif_is_netif_up(netif)) {
    return false;
  }
  esp_netif_ip_info_t ipInfo{};
  return esp_netif_get_ip_info(netif, &ipInfo) == ESP_OK &&
         ipInfo.ip.addr != 0;
}

// Any interface with an address will do, so adding Ethernet needs no
// change here.
bool networkHasIp() {
  return esp_netif_find_if(netifHasIp, nullptr) != nullptr;
}

snapclient::AudioSinkI2S::Config buildSinkConfig() {
  snapclient::AudioSinkI2S::Config sinkConfig;
  sinkConfig.bclkPin = static_cast<gpio_num_t>(CONFIG_SNAPCLIENT_I2S_BCLK_GPIO);
  sinkConfig.wsPin = static_cast<gpio_num_t>(CONFIG_SNAPCLIENT_I2S_WS_GPIO);
  sinkConfig.doutPin = static_cast<gpio_num_t>(CONFIG_SNAPCLIENT_I2S_DOUT_GPIO);
  sinkConfig.mclkPin = static_cast<gpio_num_t>(CONFIG_SNAPCLIENT_I2S_MCLK_GPIO);
  sinkConfig.mutePin = static_cast<gpio_num_t>(CONFIG_SNAPCLIENT_I2S_MUTE_GPIO);
  return sinkConfig;
}

}  // namespace

// Network receive (SnapcastClient's own task) and playback pacing (this
// task) run on separate threads, connected by a queue, so evaluate()'s
// WaitMore decisions can re-check the same chunk against real elapsed
// time.
class SnapclientTask : public bell::Task {
 public:
  // espStackOnPsram=false: no assumption that the target board has PSRAM.
  // espPriority=15 -> actual FreeRTOS priority 20 (bell::Task adds
  // CONFIG_PTHREAD_TASK_PRIO_DEFAULT=5) - above CONFIG_LWIP_TCPIP_TASK_PRIO
  // (18), so a pending WaitMore/DMA wakeup preempts lwIP instead of queuing
  // behind it. Still blocks efficiently (queue/timer/DMA waits), so idle
  // isn't starved.
  SnapclientTask()
      : bell::Task("snapclient", 8 * 1024, 15, bell::TaskCore::Core1,
                   /*espStackOnPsram=*/false) {
    startTask();
  }

  void runTask() override {
    const char* kLogTag = "snapclient_task";

    snapclient::NvsSettingsStore settingsStore;
    snapclient::ControlSettings settings(settingsStore);
    snapclient::ControlServer control(settings);

    snapclient::UdpLogBackend* udpLogBackend = nullptr;
    auto applyUdpLogSettings = [&] {
      if (udpLogBackend) {
        bell::unregisterLoggerBackend(udpLogBackend);
        udpLogBackend = nullptr;
      }
      if (settings.udpLogEnabled()) {
        auto backendRes = snapclient::UdpLogBackend::create(
            settings.udpLogHost(), settings.udpLogPort());
        if (backendRes) {
          udpLogBackend = backendRes->get();
          bell::registerLoggerBackend(std::move(*backendRes));
        } else {
          BELL_LOG(warn, kLogTag, "udp log backend failed: {}",
                   backendRes.error().message());
        }
      }
    };
    applyUdpLogSettings();

    // Null until client exists below - safe since this lambda only
    // dereferences it once a real HTTP request arrives, long after that.
    std::unique_ptr<snapclient::PlaybackPipeline> pipeline;
    control.onSettingsChanged = [&] {
      pipeline->applyDspSettings(settings.activeFlow(),
                                 settings.flowParams(settings.activeFlow()));
      applyUdpLogSettings();
    };

    auto controlListenRes = control.listen(CONFIG_SNAPCLIENT_WEB_PORT);
    if (!controlListenRes) {
      BELL_LOG(error, kLogTag, "control server listen failed: {}",
               controlListenRes.error().message());
    }

    snapclient::SnapcastClient::Config config;
    if (!settings.serverHost().empty()) {
      config.host = settings.serverHost();
      config.port = settings.serverPort();
    } else {
      config.host = CONFIG_SNAPSERVER_HOST;
      config.port = CONFIG_SNAPSERVER_PORT;
    }
    if (!settings.hostname().empty()) {
      config.clientName = settings.hostname();
    }

    // Advertising a port nothing is bound to is worse than not advertising.
    // advertise() also sets the device-wide mDNS hostname, so the board
    // answers to <clientName>.local. A later hostname change needs a reboot.
    std::unique_ptr<bell::mdns::Advertiser> httpAdvertiser;
    if (controlListenRes) {
      auto advertiseRes = bell::mdns::getDefaultManager()->advertise(
          config.clientName, "_http._tcp", "", "", CONFIG_SNAPCLIENT_WEB_PORT);
      if (advertiseRes) {
        httpAdvertiser = std::move(*advertiseRes);
        BELL_LOG(info, kLogTag, "advertising http at {}.local:{}",
                 config.clientName, CONFIG_SNAPCLIENT_WEB_PORT);
      } else {
        BELL_LOG(warn, kLogTag, "mdns advertise failed: {}",
                 advertiseRes.error().message());
      }
    }

    if (!networkHasIp()) {
      BELL_LOG(info, kLogTag,
               "waiting for a network before connecting to {}:{}", config.host,
               config.port);
      while (!networkHasIp()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
      }
    }
    snapclient::SnapcastClient client(config);
    snapclient::AudioSinkI2S i2sSink(buildSinkConfig());
    snapclient::GptimerWaiter waiter;

    pipeline = std::make_unique<snapclient::PlaybackPipeline>(
        client, i2sSink, waiter, kLogTag);
    pipeline->applyDspSettings(settings.activeFlow(),
                               settings.flowParams(settings.activeFlow()));

    client.onConnected = [&] { pipeline->onConnected(); };
    client.onServerSettings = [&](const snapclient::ServerSettings& s) {
      pipeline->onServerSettings(s);
    };
    client.onCodecReady = [&](snapclient::Codec codec,
                              const bell::audio::Format& fmt) {
      pipeline->onCodecReady(codec, fmt);
    };
    client.onTimeSample = [&](int64_t offsetUs, int64_t maxErrorUs,
                              int64_t t) {
      pipeline->onTimeSample(offsetUs, maxErrorUs, t);
    };
    client.onAudioChunk = [&](snapclient::Codec codec,
                              snapclient::ChunkBuffer payload,
                              int64_t serverTimeUs) {
      pipeline->onAudioChunk(codec, std::move(payload), serverTimeUs);
    };

    BELL_LOG(info, kLogTag, "connecting to {}:{}...", config.host,
             config.port);

    while (true) {
      pipeline->consumeOnce();
    }
  }
};

namespace {

// esp_wifi_connect() must be called after the STA netif is actually up
// (WIFI_EVENT_STA_START), and again after any disconnect - credentials
// already in flash (from a prior Improv session) get reused automatically
// by esp_wifi_connect(), no application-level persistence needed.
void onWifiEvent(void* /*arg*/, esp_event_base_t eventBase, int32_t eventId,
                 void* /*eventData*/) {
  if (eventBase == WIFI_EVENT && eventId == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (eventBase == WIFI_EVENT &&
            eventId == WIFI_EVENT_STA_DISCONNECTED) {
    // ImprovWifi::connectWifi() drives reconnection itself while
    // provisioning - step aside instead of racing it.
    if (snapclient::ImprovWifi::isProvisioning()) {
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_wifi_connect();
  } else if (eventBase == IP_EVENT && eventId == IP_EVENT_STA_GOT_IP) {
    ESP_LOGI(TAG, "WiFi got IP");
  } else if (eventBase == IP_EVENT && eventId == IP_EVENT_STA_LOST_IP) {
    // Can fire without a WIFI_EVENT_STA_DISCONNECTED (e.g. DHCP lease lost
    // while still associated), so esp_wifi_connect() isn't appropriate.
    ESP_LOGW(TAG, "WiFi lost IP");
  }
}

void wifiStationInit() {
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                             &onWifiEvent, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &onWifiEvent, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(
      IP_EVENT, IP_EVENT_STA_LOST_IP, &onWifiEvent, nullptr));

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

  if (sizeof(CONFIG_SNAPCLIENT_WIFI_SSID) > 1) {
    wifi_config_t staConfig{};
    static_assert(sizeof(CONFIG_SNAPCLIENT_WIFI_SSID) - 1 <=
                      sizeof(staConfig.sta.ssid),
                  "CONFIG_SNAPCLIENT_WIFI_SSID too long");
    static_assert(sizeof(CONFIG_SNAPCLIENT_WIFI_PASSWORD) - 1 <=
                      sizeof(staConfig.sta.password),
                  "CONFIG_SNAPCLIENT_WIFI_PASSWORD too long");
    std::memcpy(staConfig.sta.ssid, CONFIG_SNAPCLIENT_WIFI_SSID,
                sizeof(CONFIG_SNAPCLIENT_WIFI_SSID) - 1);
    std::memcpy(staConfig.sta.password, CONFIG_SNAPCLIENT_WIFI_PASSWORD,
                sizeof(CONFIG_SNAPCLIENT_WIFI_PASSWORD) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &staConfig));
    ESP_LOGI(TAG, "using compiled-in credentials for \"%s\"",
             CONFIG_SNAPCLIENT_WIFI_SSID);
  }

  ESP_ERROR_CHECK(esp_wifi_start());
  esp_wifi_set_ps(WIFI_PS_NONE);
}

}  // namespace

extern "C" void app_main(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  // Both links stay up: networkHasIp() takes whichever has an address, and
  // Improv provisioning needs the station regardless.
  wifiStationInit();
  snapclient::ethernetStart();

  // bell's logger timestamps every line with wall-clock time - without
  // this they're meaningless until the RTC happens to be right. Syncs
  // in the background once WiFi is up; doesn't block startup on it.
  setenv("TZ", CONFIG_SNAPCLIENT_SNTP_TIMEZONE, 1);
  tzset();

  esp_sntp_config_t sntpConfig =
      ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_SNAPCLIENT_SNTP_SERVER);
  if (esp_err_t sntpErr = esp_netif_sntp_init(&sntpConfig); sntpErr != ESP_OK) {
    ESP_LOGW(TAG, "esp_netif_sntp_init failed: %d", sntpErr);
  }

  bell::registerDefaultLogger();

  snapclient::scaffoldSelfCheck();

  static auto improvWifi = std::make_unique<snapclient::ImprovWifi>();
  improvWifi->onProvisioned = [] {
    ESP_LOGI(TAG, "WiFi provisioned via Improv");
  };

  static auto task = std::make_unique<SnapclientTask>();
  vTaskSuspend(NULL);
}
