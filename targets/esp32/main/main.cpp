#include <bell/Logger.h>
#include <bell/utils/Task.h>

#include <chrono>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "AudioSinkI2S.h"
#include "ImprovWifi.h"
#include "snapclient/Core.h"
#include "snapclient/DspProcessor.h"
#include "snapclient/SnapcastClient.h"
#include "snapclient/SyncEngine.h"
#include "snapclient/UdpLogBackend.h"

namespace {

const char* TAG = "snapclient";

int64_t nowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

struct QueuedChunk {
  int64_t serverTimeUs;
  std::vector<int16_t> pcm;
};

}  // namespace

// Network receive (SnapcastClient's own task) and playback pacing (this
// task) run on separate threads, connected by a queue, so evaluate()'s
// WaitMore decisions can re-check the same chunk against real elapsed
// time. No settings/discovery/HTTP control here - server host/port come
// from Kconfig.
class SnapclientTask : public bell::Task {
 public:
  // espStackOnPsram=false: no assumption that the target board has PSRAM.
  // espPriority=5: must be above tskIDLE_PRIORITY (0) - at equal priority
  // this task's idle-poll loop starves CPU1's idle task of runtime, and
  // FreeRTOS's task watchdog only expects idle to be starved briefly, not
  // continuously.
  SnapclientTask()
      : bell::Task("snapclient", 32 * 1024, 5, bell::TaskCore::Core1,
                   /*espStackOnPsram=*/false) {
    startTask();
  }

  void runTask() override {
    const char* kLogTag = "snapclient_task";

    snapclient::SyncEngine sync;
    snapclient::DspProcessor dsp;
    bell::audio::SampleRate sampleRate = bell::audio::SampleRate::SR_44100HZ;
    int32_t bufferMs = 0;

    snapclient::AudioSinkI2S::Config sinkConfig;
    sinkConfig.bclkPin =
        static_cast<gpio_num_t>(CONFIG_SNAPCLIENT_I2S_BCLK_GPIO);
    sinkConfig.wsPin = static_cast<gpio_num_t>(CONFIG_SNAPCLIENT_I2S_WS_GPIO);
    sinkConfig.doutPin =
        static_cast<gpio_num_t>(CONFIG_SNAPCLIENT_I2S_DOUT_GPIO);
    sinkConfig.mclkPin =
        static_cast<gpio_num_t>(CONFIG_SNAPCLIENT_I2S_MCLK_GPIO);
    sinkConfig.mutePin =
        static_cast<gpio_num_t>(CONFIG_SNAPCLIENT_I2S_MUTE_GPIO);
    snapclient::AudioSinkI2S i2sSink(sinkConfig);

    std::mutex queueMutex;
    std::deque<QueuedChunk> queue;

    size_t chunksPlayed = 0;
    size_t chunksDropped = 0;
    size_t corrections = 0;

    snapclient::SnapcastClient::Config config;
    config.host = CONFIG_SNAPCLIENT_SERVER_HOST;
    config.port = CONFIG_SNAPCLIENT_SERVER_PORT;
    snapclient::SnapcastClient client(config);

    client.onServerSettings = [&](const snapclient::ServerSettings& s) {
      bufferMs = s.bufferMs;
      sync.onSettingsChanged(bufferMs, 0, static_cast<uint32_t>(sampleRate));
      dsp.setVolume(static_cast<float>(s.volume) / 100.0f);
      i2sSink.setMuted(s.muted);
      BELL_LOG(info, kLogTag,
               "server settings: bufferMs={} volume={} muted={}", s.bufferMs,
               s.volume, s.muted);
    };

    client.onCodecReady = [&](const bell::audio::Format& fmt) {
      sampleRate = fmt.getSampleRate();
      sync.onSettingsChanged(bufferMs, 0, fmt.getSampleRateValue());
      i2sSink.configure(fmt.getSampleRateValue());
      BELL_LOG(info, kLogTag, "codec ready: {} Hz, {} ch",
               fmt.getSampleRateValue(), fmt.getNumChannels());
    };

    client.onTimeSample = [&](int64_t offsetUs, int64_t maxErrorUs,
                              int64_t t) {
      sync.insertLatencySample(offsetUs, maxErrorUs, t);
      if (sync.latencyReady()) {
        client.setPingIntervalUs(1000000);
      }
    };

    client.onPcmChunk = [&](const std::byte* pcm, size_t len,
                            int64_t serverTimeUs) {
      if (!sync.latencyReady()) {
        return;
      }
      QueuedChunk item;
      item.serverTimeUs = serverTimeUs;
      item.pcm.resize(len / sizeof(int16_t));
      std::memcpy(item.pcm.data(), pcm, len);

      std::lock_guard<std::mutex> lock(queueMutex);
      queue.push_back(std::move(item));
    };

    BELL_LOG(info, kLogTag, "connecting to {}:{}...", config.host,
             config.port);

    std::vector<int16_t> scratch;
    while (true) {
      QueuedChunk item;
      size_t queueDepth = 0;
      {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (queue.empty()) {
          queueDepth = 0;
        } else {
          item = std::move(queue.front());
          queue.pop_front();
          queueDepth = queue.size() + 1;
        }
      }
      if (queueDepth == 0) {
        // At CONFIG_FREERTOS_HZ=100 (10ms/tick), anything under 10ms
        // rounds down to 0 ticks - vTaskDelay(0) doesn't actually block,
        // just yields once, which isn't enough to let CPU1's idle task
        // feed the watchdog under sustained load.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }

      const size_t frames = item.pcm.size() / 2;
      scratch.resize(item.pcm.size());
      dsp.process(reinterpret_cast<const std::byte*>(item.pcm.data()),
                 item.pcm.size() * sizeof(int16_t),
                 reinterpret_cast<std::byte*>(scratch.data()),
                 scratch.size() * sizeof(int16_t), sampleRate);

      for (;;) {
        auto result = sync.evaluate(item.serverTimeUs, nowUs(), queueDepth);
        if (result.decision == snapclient::PlayDecision::WaitMore) {
          std::this_thread::sleep_for(
              std::chrono::microseconds(result.waitUs));
          continue;
        }
        if (result.decision == snapclient::PlayDecision::Play) {
          chunksPlayed++;

          constexpr size_t kBytesPerFrame = 2 * sizeof(int16_t);
          const auto* writeStart =
              reinterpret_cast<const std::byte*>(scratch.data());
          size_t writeLen = scratch.size() * sizeof(int16_t);

          // Realizes frameAdjustment on the actual I2S bytes: -1 drops the
          // last frame from this write (catch up), +1 writes it again
          // after (slow down).
          if (result.frameAdjustment < 0 && writeLen >= kBytesPerFrame) {
            writeLen -= kBytesPerFrame;
          }
          i2sSink.write(writeStart, writeLen);
          if (result.frameAdjustment > 0 && writeLen >= kBytesPerFrame) {
            i2sSink.write(writeStart + writeLen - kBytesPerFrame,
                          kBytesPerFrame);
          }

          if (result.frameAdjustment != 0) {
            corrections++;
          }
          sync.onFramesWritten(frames + result.frameAdjustment);
        } else {
          chunksDropped++;
        }
        break;
      }

      if ((chunksPlayed + chunksDropped) % 50 == 0) {
        BELL_LOG(info, kLogTag,
                 "played={} dropped={} corrections={} queued={}",
                 chunksPlayed, chunksDropped, corrections, queueDepth - 1);
      }
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
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_wifi_connect();
  } else if (eventBase == IP_EVENT && eventId == IP_EVENT_STA_GOT_IP) {
    ESP_LOGI(TAG, "WiFi got IP");
  }
}

void wifiStationInit() {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                             &onWifiEvent, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &onWifiEvent, nullptr));

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
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

  wifiStationInit();

  bell::registerDefaultLogger();

#if CONFIG_SNAPCLIENT_UDP_LOG_ENABLED
  auto udpLogRes = snapclient::UdpLogBackend::create(
      CONFIG_SNAPCLIENT_UDP_LOG_HOST, CONFIG_SNAPCLIENT_UDP_LOG_PORT);
  if (udpLogRes) {
    bell::registerLoggerBackend(std::move(*udpLogRes));
  } else {
    ESP_LOGW(TAG, "udp log backend failed: %s",
             udpLogRes.error().message().c_str());
  }
#endif

  snapclient::scaffoldSelfCheck();

  static auto improvWifi = std::make_unique<snapclient::ImprovWifi>();
  improvWifi->onProvisioned = [] {
    ESP_LOGI(TAG, "WiFi provisioned via Improv");
  };

  static auto task = std::make_unique<SnapclientTask>();
  vTaskSuspend(NULL);
}
