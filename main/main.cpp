#include <bell/Logger.h>
#include <bell/utils/Task.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

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
#include "ImprovWifi.h"
#include "NvsSettingsStore.h"
#include "snapclient/ChunkBuffer.h"
#include "snapclient/ControlServer.h"
#include "snapclient/ControlSettings.h"
#include "snapclient/Core.h"
#include "snapclient/DecoderTask.h"
#include "snapclient/DspProcessor.h"
#include "snapclient/SnapcastClient.h"
#include "snapclient/SyncEngine.h"
#include "snapclient/UdpLogBackend.h"
#include "snapclient/DynamicResampler.h"

namespace {

const char* TAG = "snapclient";

std::atomic<bool> wifiConnected{false};

// Bounds the network-to-playback queue. A chunk acquireChunkBuffer()
// couldn't get memory for still carries useful timing (it falls back to
// the accounted-silence path in onAudioChunk below), so this isn't tied
// to any particular memory budget - just high enough to absorb ordinary
// network jitter without starving the queue into unaccounted drops.
constexpr size_t kQueueCapacity = 40;

// Decode (~12ms avg) is slower than dsp+i2s-write (~6.5-7.5ms combined),
// so 2 slots is enough for one-chunk-ahead overlap between DecoderTask
// and the consumer; bump if soak testing shows the consumer stalling
// here waiting on decode.
constexpr size_t kPcmQueueCapacity = 2;

int64_t nowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

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
      : bell::Task("snapclient", 8 * 1024, 5, bell::TaskCore::Core1,
                   /*espStackOnPsram=*/false) {
    startTask();
  }

  void runTask() override {
    const char* kLogTag = "snapclient_task";

    snapclient::SyncEngine sync(kQueueCapacity);
    snapclient::DspProcessor dsp;
    bell::audio::SampleRate sampleRate = bell::audio::SampleRate::SR_44100HZ;
    int32_t bufferMs = 0;
    // Snapcast's per-client "latency" setting, which nothing on this end
    // can measure directly.
    int32_t dacFixedLatencyMs = 0;
    // Tracks what SyncEngine was last told, so a ServerSettings message
    // that only changed volume/mute (bundled together in the same message
    // by the protocol) doesn't also force a resync - sync.onSettingsChanged
    // drops playing_ back to the initial-sync state, which costs several
    // seconds to recover from and has nothing to do with volume.
    int32_t lastSyncBufferMs = 0;
    int64_t lastServerSettingsLogUs = 0;

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

    snapclient::BoundedQueue<snapclient::QueuedChunk> queue(kQueueCapacity);
    snapclient::BoundedQueue<snapclient::DecodedChunk> pcmQueue(
        kPcmQueueCapacity);
    std::atomic<uint32_t> codecGeneration{0};

    size_t chunksPlayed = 0;
    size_t chunksDropped = 0;
    // Split by direction: a real clock-rate bias grows one side much
    // faster than the other, unlike threshold noise on both.
    size_t correctionsSkip = 0;
    size_t correctionsDuplicate = 0;

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
      dacFixedLatencyMs = s.latencyMs;

      if (bufferMs != lastSyncBufferMs) {
        lastSyncBufferMs = bufferMs;
        sync.onSettingsChanged(bufferMs, static_cast<uint32_t>(sampleRate));
      }
      dsp.setVolume(static_cast<float>(s.volume) / 100.0f);
      i2sSink.setMuted(s.muted);

      // A UI volume slider sends one of these per tick while dragging -
      // same UART-stall reasoning as the drop log above for throttling it.
      const int64_t settingsLogNow = nowUs();
      if (settingsLogNow - lastServerSettingsLogUs >= 1000000) {
        lastServerSettingsLogUs = settingsLogNow;
        BELL_LOG(info, kLogTag,
                 "server settings: bufferMs={} volume={} muted={}",
                 s.bufferMs, s.volume, s.muted);
      }
    };

    client.onCodecReady = [&](snapclient::Codec /*codec*/,
                              const bell::audio::Format& fmt) {
      sampleRate = fmt.getSampleRate();
      i2sSink.configure(fmt.getSampleRateValue());
      lastSyncBufferMs = bufferMs;
      sync.onSettingsChanged(bufferMs, fmt.getSampleRateValue());
      // Queued chunks predate this codec header and would otherwise
      // decode against the *new* decoder instance setupDecode() just
      // recreated in SnapcastClient - bump codecGeneration first so
      // DecoderTask can also recognize and drop anything already popped
      // but not yet decoded/published.
      codecGeneration.fetch_add(1, std::memory_order_relaxed);
      queue.clear();
      pcmQueue.clear();
      BELL_LOG(info, kLogTag, "codec ready: {} Hz, {} ch",
               fmt.getSampleRateValue(), fmt.getNumChannels());
    };

    client.onTimeSample = [&](int64_t offsetUs, int64_t maxErrorUs,
                              int64_t t) {
      sync.insertLatencySample(offsetUs, maxErrorUs, t);
      if (sync.latencyReady()) {
        // A shorter interval keeps TimeFilter's offset fresher, but adds
        // network-thread load that can nudge queue depth past what the
        // pool backs with real encoded buffers - 1s keeps that load low.
        client.setPingIntervalUs(1000000);
      }
    };

    int64_t lastDropLogUs = 0;
    size_t queueFullDrops = 0;

    client.onAudioChunk = [&](snapclient::Codec codec, const std::byte* payload,
                              size_t len, int64_t serverTimeUs) {
      if (!sync.latencyReady()) {
        return;
      }
      snapclient::QueuedChunk item;
      item.serverTimeUs = serverTimeUs;
      item.codec = codec;
      item.codecGen = codecGeneration.load(std::memory_order_relaxed);
      // An allocation failure queues a silent placeholder (item.payload
      // left empty) instead of dropping the chunk outright - a dropped
      // chunk leaves a gap in the played frame count that the
      // server's clock doesn't have. See DecoderTask's silencePlayed log
      // for how often this actually results in audible silence.
      item.payload = snapclient::acquireChunkBuffer(payload, len);
      if (!queue.tryPush(std::move(item))) {
        queueFullDrops++;
      }

      // Sustained overload logs every chunk otherwise - the blocking
      // UART write itself then becomes part of the overload, up to
      // starving other tasks long enough to trip the watchdog. 30s, not
      // 1s: an occasional single dropped chunk is normal jitter, not
      // something worth a line every second.
      const int64_t dropLogNow = nowUs();
      if (dropLogNow - lastDropLogUs >= 30000000) {
        lastDropLogUs = dropLogNow;
        if (queueFullDrops > 0) {
          BELL_LOG(warn, kLogTag, "dropped {} chunks (queue full) in the last 30s",
                   queueFullDrops);
          queueFullDrops = 0;
        }
      }
    };

    snapclient::DecoderTask decoder(queue, pcmQueue, codecGeneration, client);

    BELL_LOG(info, kLogTag, "connecting to {}:{}...", config.host,
             config.port);

    std::vector<int16_t> scratch;
    std::vector<int16_t> scratch_resampled;
    
    constexpr int64_t kBackgroundLogIntervalUs = 10'000'000;  // 10s
    int64_t lastQueueLogUs = 0;
    int64_t lastStackLogUs = 0;
    int64_t lastPlayedLogUs = 0;
    
    int64_t lastResyncAtUs = nowUs();
    size_t lastCorrectionsSkipLogged = 0;
    size_t lastCorrectionsDuplicateLogged = 0;
    int64_t lastAgeUs = 0;
    int64_t lastDiffToServerUs = 0;
    uint32_t lastUnderrunCompensationFrames = 0;

    while (true) {
      snapclient::DecodedChunk item;
      size_t queueDepth = 0;
      if (pcmQueue.tryPop(item, 10)) {
        queueDepth = queue.size() + pcmQueue.size() + 1;
      }

      const int64_t logNow = nowUs();
      if (logNow - lastQueueLogUs >= kBackgroundLogIntervalUs) {
        lastQueueLogUs = logNow;
        // A growing gap between freeHeap and largestFreeBlock over time
        // is the fragmentation signal to watch for.
        BELL_LOG(info, kLogTag,
                 "queue depth={} pcmQueue depth={} freeHeap={} "
                 "largestFreeBlock={}",
                 queue.size(), pcmQueue.size(), esp_get_free_heap_size(),
                 snapclient::chunkHeapLargestFreeBlockBytes());
      }

      // Stack sizes below are all fixed guesses, not measured - this
      // reports real headroom so they can be right-sized instead of
      // guessed again.
      if (logNow - lastStackLogUs >= 10000000) {
        lastStackLogUs = logNow;
        BELL_LOG(info, kLogTag,
                 "stack headroom: snapclient_task={}B snapcast_client={}B "
                 "decoder_task={}B",
                 getStackHighWaterMarkWords() * sizeof(StackType_t),
                 client.stackHighWaterMarkWords() * sizeof(StackType_t),
                 decoder.stackHighWaterMarkWords() * sizeof(StackType_t));
      }

      if (queueDepth == 0) {
        // tryPop() already waited up to 10ms internally on a miss -
        // woken early as soon as DecoderTask pushes something.
        continue;
      }

      const std::byte* dspInput = item.pcm.data();
      size_t dspInputLen = item.pcm.size();
      size_t frames = dspInputLen / snapclient::kBytesPerFrame;
      scratch.resize(dspInputLen / sizeof(int16_t));
      dsp.process(dspInput, dspInputLen,
                 reinterpret_cast<std::byte*>(scratch.data()),
                 scratch.size() * sizeof(int16_t), sampleRate);

      const int32_t dacLatencyUs =
          static_cast<int32_t>(dacFixedLatencyMs) * 1000;

      // Captured once before the retry loop below, not per-iteration:
      // WaitMore re-evaluates the same not-yet-playing chunk without ever
      // flipping playing_, so isPlaying() can't change mid-loop.
      const bool wasPlayingBeforeEval = sync.isPlaying();

      for (;;) {
        const int64_t evalStartUs = nowUs();
        auto result = sync.evaluate(item.serverTimeUs, evalStartUs, queueDepth,
                                    dacLatencyUs);

        if (result.decision == snapclient::PlayDecision::WaitMore) {
          std::this_thread::sleep_for(
              std::chrono::microseconds(result.waitUs));
          continue;
        }
        if (result.decision == snapclient::PlayDecision::Play) {
          chunksPlayed++;

          // frameAdjustment is already scaled - see
          // SyncEngine::scaleFrameAdjustment(). Resampled across the whole
          // chunk, not a single-frame drop/duplicate at the edge, since
          // the magnitude can now exceed one frame.
          const size_t targetFrames =
              static_cast<size_t>(static_cast<int>(frames) +
                                  result.frameAdjustment);
          scratch_resampled.resize(targetFrames * 2);  // stereo
          snapclient::DynamicResampler::process(
              reinterpret_cast<const int16_t*>(scratch.data()), frames,
              scratch_resampled.data(), targetFrames);

          i2sSink.write(
              reinterpret_cast<const std::byte*>(scratch_resampled.data()),
              targetFrames * snapclient::kBytesPerFrame);

          if (result.frameAdjustment < 0) {
            correctionsSkip++;
          } else if (result.frameAdjustment > 0) {
            correctionsDuplicate++;
          }

          // Tell SyncEngine the actual frames just written
          sync.onFramesWritten(targetFrames);

          // Underruns advance the DAC's clock without a matching write() -
          // feed that into SyncEngine too. Only while already playing: a
          // resync's own search leaves the DMA idle on purpose, that's not
          // lost time.
          const uint32_t underrunFrames = i2sSink.underrunCompensationFrames();
          if (wasPlayingBeforeEval) {
            const uint32_t underrunDelta =
                underrunFrames - lastUnderrunCompensationFrames;
            if (underrunDelta > 0) {
              sync.onFramesWritten(underrunDelta);
            }
          }
          lastUnderrunCompensationFrames = underrunFrames;

          // shortMedian_ isn't full on the very first chunk(s) after a
          // resync, so evaluate() reports ageUs=0/diffToServerUs=0 then -
          // harmless here since the drift log below only cares about the
          // steady-state trend over minutes/hours, not that transient.
          lastAgeUs = result.ageUs;
          lastDiffToServerUs = result.diffToServerUs;
        } else {
          chunksDropped++;

          // Yield briefly so we don't starve other tasks
          std::this_thread::yield();

          if (!sync.isPlaying()) {
            // Still resyncing after this drop - the rest of the backlog
            // only gets staler evaluated one at a time in FIFO order,
            // since real time keeps advancing while working through it.
            // Jump straight to the newest chunk instead, on both queues.
            chunksDropped += pcmQueue.drainToNewest(1);
            chunksDropped += queue.drainToNewest(1);
          }
        }
        break;
      }

      // playing_ went false -> true: evaluate() just reset
      // playbackStartTimeUs_/samplesWritten_ (see SyncEngine.cpp), from a
      // hard resync or a fresh connection.
      if (!wasPlayingBeforeEval && sync.isPlaying()) {
        lastResyncAtUs = nowUs();
      }

      const int64_t timingLogNow = nowUs();

      if (timingLogNow - lastPlayedLogUs >= kBackgroundLogIntervalUs) {
        lastPlayedLogUs = timingLogNow;
        // skip/dup are deltas since the last log, not running totals -
        // those lose resolution over a multi-hour soak.
        const int64_t deltaSkip =
            static_cast<int64_t>(correctionsSkip - lastCorrectionsSkipLogged);
        const int64_t deltaDuplicate = static_cast<int64_t>(
            correctionsDuplicate - lastCorrectionsDuplicateLogged);
        lastCorrectionsSkipLogged = correctionsSkip;
        lastCorrectionsDuplicateLogged = correctionsDuplicate;
        BELL_LOG(info, kLogTag,
                 "played={} dropped={} queued={} i2sSendQOverflow={} "
                 "ageUs={} diffToServerUs={} netCorrection={} (skip={} "
                 "dup={}) playingForS={}",
                 chunksPlayed, chunksDropped, queueDepth - 1,
                 i2sSink.sendQueueOverflowCount(), lastAgeUs,
                 lastDiffToServerUs, deltaSkip - deltaDuplicate, deltaSkip,
                 deltaDuplicate, (timingLogNow - lastResyncAtUs) / 1000000);
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
    // ImprovWifi::connectWifi() drives reconnection itself while
    // provisioning - step aside instead of racing it.
    if (snapclient::ImprovWifi::isProvisioning()) {
      return;
    }
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
