#include <bell/Logger.h>
#include <bell/utils/Task.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_pm.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "AudioSinkI2S.h"
#include "ImprovWifi.h"
#include "NvsSettingsStore.h"
#include "PcmChunkPool.h"
#include "snapclient/ControlServer.h"
#include "snapclient/ControlSettings.h"
#include "snapclient/Core.h"
#include "snapclient/DspProcessor.h"
#include "snapclient/SnapcastClient.h"
#include "snapclient/SyncEngine.h"
#include "snapclient/UdpLogBackend.h"

namespace {

const char* TAG = "snapclient";

std::atomic<bool> wifiConnected{false};

// 15 slots of 4096 bytes (~61KB, ~300ms of 48kHz stereo S16 audio),
// pre-allocated once at startup instead of per-chunk to avoid heap
// fragmentation under sustained ~50 allocs/frees per second.
//
// PcmChunkPool prefers IRAM (a pool separate from the DRAM heap
// Opus/DSP/WiFi/HTTP draw from) but falls back to DRAM when IRAM has no
// room - this board's IRAM is fully committed elsewhere, so this is
// sized against DRAM headroom; sizing it larger starves those other
// allocations.
constexpr size_t kPcmPoolSlotCount = 15;
constexpr size_t kPcmPoolSlotBytes = 4096;

// Snapcast's Opus stream here always encodes 20ms frames at 48kHz -
// 960 samples per channel. Used to size the silence placeholder written
// in place of a chunk the pool couldn't hold.
constexpr size_t kFramesPerChunk = 960;

int64_t nowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

struct QueuedChunk {
  int64_t serverTimeUs;
  snapclient::PcmChunkPool::PooledBuffer pcm;
};

}  // namespace

// Network receive (SnapcastClient's own task) and playback pacing (this
// task) run on separate threads, connected by a queue, so evaluate()'s
// WaitMore decisions can re-check the same chunk against real elapsed
// time.
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

    snapclient::NvsSettingsStore settingsStore;
    snapclient::ControlSettings settings(settingsStore);
    snapclient::ControlServer control(settings);

    dsp.switchFlow(settings.activeFlow());
    dsp.setParams(settings.activeFlow(),
                 settings.flowParams(settings.activeFlow()));

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

    control.onSettingsChanged = [&] {
      dsp.switchFlow(settings.activeFlow());
      dsp.setParams(settings.activeFlow(),
                    settings.flowParams(settings.activeFlow()));
      applyUdpLogSettings();
    };

    auto controlListenRes = control.listen(CONFIG_SNAPCLIENT_CONTROL_PORT);
    if (!controlListenRes) {
      BELL_LOG(error, kLogTag, "control server listen failed: {}",
               controlListenRes.error().message());
    }

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

    snapclient::PcmChunkPool pcmPool(kPcmPoolSlotCount, kPcmPoolSlotBytes);
    std::mutex queueMutex;
    std::deque<QueuedChunk> queue;

    size_t chunksPlayed = 0;
    size_t chunksDropped = 0;
    size_t corrections = 0;

    // Held only while frames are going to the DAC, so DFS can drop the
    // clock the rest of the time. Null handle (e.g. PM disabled) just
    // makes acquire/release below no-ops.
    esp_pm_lock_handle_t pmLock = nullptr;
    if (esp_err_t pmErr = esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0,
                                             "snapclient_playback", &pmLock);
        pmErr != ESP_OK) {
      BELL_LOG(warn, kLogTag, "esp_pm_lock_create failed: {}", pmErr);
      pmLock = nullptr;
    }
    bool playbackActive = false;

    snapclient::SnapcastClient::Config config;
    if (!settings.serverHost().empty()) {
      config.host = settings.serverHost();
      config.port = settings.serverPort();
    } else {
      config.host = CONFIG_SNAPCLIENT_SERVER_HOST;
      config.port = CONFIG_SNAPCLIENT_SERVER_PORT;
    }
    if (!settings.hostname().empty()) {
      config.clientName = settings.hostname();
    }
    if (!wifiConnected) {
      BELL_LOG(info, kLogTag, "waiting for WiFi before connecting to {}:{}",
               config.host, config.port);
      while (!wifiConnected) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
      }
    }
    snapclient::SnapcastClient client(config);

    client.onConnected = [&] { sync.reset(); };

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
      std::lock_guard<std::mutex> lock(queueMutex);
      if (queue.size() >= kPcmPoolSlotCount) {
        BELL_LOG(warn, kLogTag, "dropping pcm chunk: queue full");
        return;
      }

      QueuedChunk item;
      item.serverTimeUs = serverTimeUs;
      item.pcm = pcmPool.acquire(pcm, len);
      // Pool exhaustion queues a silent placeholder (item.pcm left empty)
      // instead of dropping the chunk outright - a dropped chunk leaves a
      // gap in the played frame count that the server's clock doesn't
      // have, which is what was driving SyncEngine's runaway resyncs.
      if (!item.pcm) {
        BELL_LOG(warn, kLogTag, "pool exhausted, queueing silence");
      }
      queue.push_back(std::move(item));
    };

    BELL_LOG(info, kLogTag, "connecting to {}:{}...", config.host,
             config.port);

    std::vector<int16_t> scratch;
    const std::vector<int16_t> silenceInput(kFramesPerChunk * 2, 0);
    int64_t lastQueueLogUs = 0;
    int64_t lastTimingLogUs = 0;
    int64_t dspSumUs = 0, dspMaxUs = 0;
    int64_t i2sSumUs = 0, i2sMaxUs = 0;
    size_t dspSamples = 0, i2sSamples = 0;
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

      const int64_t logNow = nowUs();
      if (logNow - lastQueueLogUs >= 1000000) {
        lastQueueLogUs = logNow;
        BELL_LOG(info, kLogTag, "queue depth={} freeHeap={}", queueDepth,
                 esp_get_free_heap_size());
      }

      if (queueDepth == 0) {
        if (pmLock != nullptr && playbackActive) {
          esp_pm_lock_release(pmLock);
          playbackActive = false;
        }
        // At CONFIG_FREERTOS_HZ=100 (10ms/tick), anything under 10ms
        // rounds down to 0 ticks - vTaskDelay(0) doesn't actually block,
        // just yields once, which isn't enough to let CPU1's idle task
        // feed the watchdog under sustained load.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }

      constexpr size_t kBytesPerFrame = 2 * sizeof(int16_t);
      const std::byte* dspInput;
      size_t dspInputLen;
      size_t frames;
      if (item.pcm) {
        dspInput = item.pcm.data();
        dspInputLen = item.pcm.size();
        frames = dspInputLen / kBytesPerFrame;
      } else {
        dspInput = reinterpret_cast<const std::byte*>(silenceInput.data());
        dspInputLen = silenceInput.size() * sizeof(int16_t);
        frames = kFramesPerChunk;
      }
      scratch.resize(dspInputLen / sizeof(int16_t));
      const int64_t dspStartUs = nowUs();
      dsp.process(dspInput, dspInputLen,
                 reinterpret_cast<std::byte*>(scratch.data()),
                 scratch.size() * sizeof(int16_t), sampleRate);
      const int64_t dspUs = nowUs() - dspStartUs;
      dspSumUs += dspUs;
      dspMaxUs = std::max(dspMaxUs, dspUs);
      dspSamples++;

      for (;;) {
        auto result = sync.evaluate(item.serverTimeUs, nowUs(), queueDepth);
        if (result.decision == snapclient::PlayDecision::WaitMore) {
          std::this_thread::sleep_for(
              std::chrono::microseconds(result.waitUs));
          continue;
        }
        if (result.decision == snapclient::PlayDecision::Play) {
          chunksPlayed++;

          if (pmLock != nullptr && !playbackActive) {
            esp_pm_lock_acquire(pmLock);
            playbackActive = true;
          }

          const auto* writeStart =
              reinterpret_cast<const std::byte*>(scratch.data());
          size_t writeLen = scratch.size() * sizeof(int16_t);

          // Realizes frameAdjustment on the actual I2S bytes: -1 drops the
          // last frame from this write (catch up), +1 writes it again
          // after (slow down).
          if (result.frameAdjustment < 0 && writeLen >= kBytesPerFrame) {
            writeLen -= kBytesPerFrame;
          }
          const int64_t i2sStartUs = nowUs();
          i2sSink.write(writeStart, writeLen);
          if (result.frameAdjustment > 0 && writeLen >= kBytesPerFrame) {
            i2sSink.write(writeStart + writeLen - kBytesPerFrame,
                          kBytesPerFrame);
          }
          const int64_t i2sUs = nowUs() - i2sStartUs;
          i2sSumUs += i2sUs;
          i2sMaxUs = std::max(i2sMaxUs, i2sUs);
          i2sSamples++;

          if (result.frameAdjustment != 0) {
            corrections++;
          }
          sync.onFramesWritten(frames + result.frameAdjustment);
        } else {
          chunksDropped++;
        }
        break;
      }

      const int64_t timingLogNow = nowUs();
      if (timingLogNow - lastTimingLogUs >= 1000000) {
        lastTimingLogUs = timingLogNow;
        BELL_LOG(info, kLogTag,
                 "loop timing: dspAvg={} dspMax={} n={} | i2sAvg={} "
                 "i2sMax={} n={}",
                 dspSamples ? dspSumUs / dspSamples : 0, dspMaxUs,
                 dspSamples, i2sSamples ? i2sSumUs / i2sSamples : 0,
                 i2sMaxUs, i2sSamples);
        dspSumUs = 0;
        dspMaxUs = 0;
        dspSamples = 0;
        i2sSumUs = 0;
        i2sMaxUs = 0;
        i2sSamples = 0;
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
    wifiConnected = false;
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_wifi_connect();
  } else if (eventBase == IP_EVENT && eventId == IP_EVENT_STA_GOT_IP) {
    wifiConnected = true;
    ESP_LOGI(TAG, "WiFi got IP");
  } else if (eventBase == IP_EVENT && eventId == IP_EVENT_STA_LOST_IP) {
    // Can fire without a WIFI_EVENT_STA_DISCONNECTED (e.g. DHCP lease lost
    // while still associated) - esp_wifi_connect() isn't appropriate here,
    // just stop treating the link as usable until GOT_IP fires again.
    wifiConnected = false;
    ESP_LOGW(TAG, "WiFi lost IP");
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
  ESP_ERROR_CHECK(esp_event_handler_register(
      IP_EVENT, IP_EVENT_STA_LOST_IP, &onWifiEvent, nullptr));

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

  // DFS + light sleep: idle most of the runtime between chunks, so let
  // the clock drop instead of sitting at max always.
  esp_pm_config_t pmConfig = {
      .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
      .min_freq_mhz = 40,
      .light_sleep_enable = true,
  };
  if (esp_err_t pmErr = esp_pm_configure(&pmConfig); pmErr != ESP_OK) {
    ESP_LOGW(TAG, "esp_pm_configure failed: %d", pmErr);
  }

  wifiStationInit();

  // bell's logger timestamps every line with wall-clock time - without
  // this they're meaningless until the RTC happens to be right. Syncs
  // in the background once WiFi is up; doesn't block startup on it.
  esp_sntp_config_t sntpConfig = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
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
