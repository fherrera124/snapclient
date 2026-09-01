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

class RateLimiter {
 public:
  explicit RateLimiter(int64_t intervalUs) : intervalUs_(intervalUs) {}
  bool due(int64_t nowUs) {
    if (nowUs - lastUs_ < intervalUs_) {
      return false;
    }
    lastUs_ = nowUs;
    return true;
  }

 private:
  int64_t intervalUs_;
  int64_t lastUs_ = 0;
};

struct PlaybackStats {
  size_t chunksPlayed = 0;
  size_t chunksDropped = 0;
  // Split by direction: a real clock-rate bias grows one side much
  // faster than the other, unlike threshold noise on both.
  size_t correctionsSkip = 0;
  size_t correctionsDuplicate = 0;
  size_t lastCorrectionsSkipLogged = 0;
  size_t lastCorrectionsDuplicateLogged = 0;
  int64_t lastAgeUs = 0;
  int64_t lastDiffToServerUs = 0;
  int64_t lastResyncAtUs = nowUs();
  uint32_t lastUnderrunCompensationFrames = 0;

  void maybeLogSummary(RateLimiter& limiter, const char* logTag, int64_t now,
                       size_t queueDepth, uint32_t i2sOverflow) {
    if (!limiter.due(now)) {
      return;
    }
    // skip/dup are deltas since the last log, not running totals - those
    // lose resolution over a multi-hour soak.
    const int64_t deltaSkip =
        static_cast<int64_t>(correctionsSkip - lastCorrectionsSkipLogged);
    const int64_t deltaDuplicate = static_cast<int64_t>(
        correctionsDuplicate - lastCorrectionsDuplicateLogged);
    lastCorrectionsSkipLogged = correctionsSkip;
    lastCorrectionsDuplicateLogged = correctionsDuplicate;
    BELL_LOG(info, logTag,
             "played={} dropped={} queued={} i2sSendQOverflow={} "
             "ageUs={} diffToServerUs={} netCorrection={} (skip={} "
             "dup={}) playingForS={}",
             chunksPlayed, chunksDropped, queueDepth - 1, i2sOverflow,
             lastAgeUs, lastDiffToServerUs, deltaSkip - deltaDuplicate,
             deltaSkip, deltaDuplicate, (now - lastResyncAtUs) / 1000000);
  }
};

snapclient::AudioSinkI2S::Config buildSinkConfig() {
  snapclient::AudioSinkI2S::Config sinkConfig;
  sinkConfig.bclkPin = static_cast<gpio_num_t>(CONFIG_SNAPCLIENT_I2S_BCLK_GPIO);
  sinkConfig.wsPin = static_cast<gpio_num_t>(CONFIG_SNAPCLIENT_I2S_WS_GPIO);
  sinkConfig.doutPin = static_cast<gpio_num_t>(CONFIG_SNAPCLIENT_I2S_DOUT_GPIO);
  sinkConfig.mclkPin = static_cast<gpio_num_t>(CONFIG_SNAPCLIENT_I2S_MCLK_GPIO);
  sinkConfig.mutePin = static_cast<gpio_num_t>(CONFIG_SNAPCLIENT_I2S_MUTE_GPIO);
  return sinkConfig;
}

// One class, not two: the five SnapcastClient callbacks and consumeOnce()
// mutate the same state (sync_/queue_/pcmQueue_/bufferMs_), so splitting
// them would just add a layer holding references to it. DecoderTask lives
// here too, so nothing needs a public accessor just to wire it up.
//
// The five onXxx methods run on SnapcastClient's own thread; consumeOnce()
// runs on the caller's - plain reads/writes across that boundary, no
// synchronization.
class PlaybackPipeline {
 public:
  PlaybackPipeline(snapclient::SnapcastClient& client, const char* logTag)
      : logTag_(logTag),
        sync_(kQueueCapacity),
        i2sSink_(buildSinkConfig()),
        queue_(kQueueCapacity),
        pcmQueue_(kPcmQueueCapacity),
        decoder_(queue_, pcmQueue_, codecGeneration_, client),
        client_(client),
        serverSettingsLogLimiter_(1'000'000),
        dropLogLimiter_(30'000'000) {}

  void applyDspSettings(snapclient::DspFlow flow,
                        const snapclient::DspFilterParams& params) {
    dsp_.switchFlow(flow);
    dsp_.setParams(flow, params);
  }

  void onConnected() { sync_.reset(); }

  void onServerSettings(const snapclient::ServerSettings& s) {
    bufferMs_ = s.bufferMs;
    dacFixedLatencyMs_ = s.latencyMs;

    if (bufferMs_ != lastSyncBufferMs_) {
      lastSyncBufferMs_ = bufferMs_;
      sync_.onSettingsChanged(bufferMs_, static_cast<uint32_t>(sampleRate_));
    }
    dsp_.setVolume(static_cast<float>(s.volume) / 100.0f);
    i2sSink_.setMuted(s.muted);

    // A UI volume slider sends one of these per tick while dragging -
    // same UART-stall reasoning as the drop log below for throttling it.
    if (serverSettingsLogLimiter_.due(nowUs())) {
      BELL_LOG(info, logTag_,
               "server settings: bufferMs={} volume={} muted={}", s.bufferMs,
               s.volume, s.muted);
    }
  }

  void onCodecReady(snapclient::Codec /*codec*/,
                    const bell::audio::Format& fmt) {
    sampleRate_ = fmt.getSampleRate();
    i2sSink_.configure(fmt.getSampleRateValue());
    lastSyncBufferMs_ = bufferMs_;
    sync_.onSettingsChanged(bufferMs_, fmt.getSampleRateValue());
    // Queued chunks predate this codec header and would otherwise
    // decode against the *new* decoder instance setupDecode() just
    // recreated in SnapcastClient - bump codecGeneration first so
    // DecoderTask can also recognize and drop anything already popped
    // but not yet decoded/published.
    codecGeneration_.fetch_add(1, std::memory_order_relaxed);
    queue_.clear();
    pcmQueue_.clear();
    BELL_LOG(info, logTag_, "codec ready: {} Hz, {} ch",
             fmt.getSampleRateValue(), fmt.getNumChannels());
  }

  void onTimeSample(int64_t offsetUs, int64_t maxErrorUs, int64_t t) {
    sync_.insertLatencySample(offsetUs, maxErrorUs, t);
    if (sync_.latencyReady()) {
      // A shorter interval keeps TimeFilter's offset fresher, but adds
      // network-thread load that can nudge queue depth past what the
      // pool backs with real encoded buffers - 1s keeps that load low.
      client_.setPingIntervalUs(1000000);
    }
  }

  void onAudioChunk(snapclient::Codec codec, const std::byte* payload,
                    size_t len, int64_t serverTimeUs) {
    if (!sync_.latencyReady()) {
      return;
    }
    snapclient::QueuedChunk item;
    item.serverTimeUs = serverTimeUs;
    item.codec = codec;
    item.codecGen = codecGeneration_.load(std::memory_order_relaxed);
    // An allocation failure queues a silent placeholder (item.payload
    // left empty) instead of dropping the chunk outright - a dropped
    // chunk leaves a gap in the played frame count that the
    // server's clock doesn't have. See DecoderTask's silencePlayed log
    // for how often this actually results in audible silence.
    item.payload = snapclient::acquireChunkBuffer(payload, len);
    if (!queue_.tryPush(std::move(item))) {
      queueFullDrops_++;
    }

    // Sustained overload logs every chunk otherwise - the blocking
    // UART write itself then becomes part of the overload, up to
    // starving other tasks long enough to trip the watchdog. 30s, not
    // 1s: an occasional single dropped chunk is normal jitter, not
    // something worth a line every second.
    if (dropLogLimiter_.due(nowUs())) {
      if (queueFullDrops_ > 0) {
        BELL_LOG(warn, logTag_,
                 "dropped {} chunks (queue full) in the last 30s",
                 queueFullDrops_);
        queueFullDrops_ = 0;
      }
    }
  }

  // Pops and plays (or drops) one chunk, or returns immediately if none
  // are ready yet. Call in a tight loop.
  void consumeOnce(PlaybackStats& stats) {
    snapclient::DecodedChunk item;
    size_t queueDepth = 0;
    if (pcmQueue_.tryPop(item, 10)) {
      queueDepth = queue_.size() + pcmQueue_.size() + 1;
    }
    if (queueDepth == 0) {
      // tryPop() already waited up to 10ms internally on a miss -
      // woken early as soon as DecoderTask pushes something.
      return;
    }

    const std::byte* dspInput = item.pcm.data();
    size_t dspInputLen = item.pcm.size();
    size_t frames = dspInputLen / snapclient::kBytesPerFrame;
    scratch_.resize(dspInputLen / sizeof(int16_t));
    dsp_.process(dspInput, dspInputLen,
                reinterpret_cast<std::byte*>(scratch_.data()),
                scratch_.size() * sizeof(int16_t), sampleRate_);

    const int32_t dacLatencyUs =
        static_cast<int32_t>(dacFixedLatencyMs_) * 1000;

    // Captured once before the retry loop below, not per-iteration:
    // WaitMore re-evaluates the same not-yet-playing chunk without ever
    // flipping playing_, so isPlaying() can't change mid-loop.
    const bool wasPlayingBeforeEval = sync_.isPlaying();

    for (;;) {
      const int64_t evalStartUs = nowUs();
      auto result = sync_.evaluate(item.serverTimeUs, evalStartUs, queueDepth,
                                   dacLatencyUs);

      if (result.decision == snapclient::PlayDecision::WaitMore) {
        std::this_thread::sleep_for(
            std::chrono::microseconds(result.waitUs));
        continue;
      }
      if (result.decision == snapclient::PlayDecision::Play) {
        stats.chunksPlayed++;

        // frameAdjustment is already scaled - see
        // SyncEngine::scaleFrameAdjustment(). Resampled across the whole
        // chunk, not a single-frame drop/duplicate at the edge, since
        // the magnitude can now exceed one frame.
        const size_t targetFrames =
            static_cast<size_t>(static_cast<int>(frames) +
                                result.frameAdjustment);
        scratchResampled_.resize(targetFrames * 2);  // stereo
        snapclient::DynamicResampler::process(
            reinterpret_cast<const int16_t*>(scratch_.data()), frames,
            scratchResampled_.data(), targetFrames);

        i2sSink_.write(
            reinterpret_cast<const std::byte*>(scratchResampled_.data()),
            targetFrames * snapclient::kBytesPerFrame);

        if (result.frameAdjustment < 0) {
          stats.correctionsSkip++;
        } else if (result.frameAdjustment > 0) {
          stats.correctionsDuplicate++;
        }

        // Tell SyncEngine the actual frames just written
        sync_.onFramesWritten(targetFrames);

        // Underruns advance the DAC's clock without a matching write() -
        // feed that into SyncEngine too. Only while already playing: a
        // resync's own search leaves the DMA idle on purpose, that's not
        // lost time.
        const uint32_t underrunFrames = i2sSink_.underrunCompensationFrames();
        if (wasPlayingBeforeEval) {
          const uint32_t underrunDelta =
              underrunFrames - stats.lastUnderrunCompensationFrames;
          if (underrunDelta > 0) {
            sync_.onFramesWritten(underrunDelta);
          }
        }
        stats.lastUnderrunCompensationFrames = underrunFrames;

        // shortMedian_ isn't full on the very first chunk(s) after a
        // resync, so evaluate() reports ageUs=0/diffToServerUs=0 then -
        // harmless here since the drift log below only cares about the
        // steady-state trend over minutes/hours, not that transient.
        stats.lastAgeUs = result.ageUs;
        stats.lastDiffToServerUs = result.diffToServerUs;
      } else {
        stats.chunksDropped++;

        // Yield briefly so we don't starve other tasks
        std::this_thread::yield();

        if (!sync_.isPlaying()) {
          // Still resyncing after this drop - the rest of the backlog
          // only gets staler evaluated one at a time in FIFO order,
          // since real time keeps advancing while working through it.
          // Jump straight to the newest chunk instead, on both queues.
          stats.chunksDropped += pcmQueue_.drainToNewest(1);
          stats.chunksDropped += queue_.drainToNewest(1);
        }
      }
      break;
    }

    // Frame-level correction can't shrink queue.size() by a whole chunk,
    // so backlog that never trips a hard resync otherwise never drains.
    // Compensates via onFramesWritten(), same as underrun does above.
    if (wasPlayingBeforeEval) {
      const int64_t queueExcessCheckNowUs = nowUs();
      const size_t targetQueue = static_cast<size_t>(bufferMs_ / 20);
      // Below ~220ms bufferMs, targetQueue-8 would underflow - disable
      // rather than let a wrapped size_t make the check silently inert.
      if (targetQueue > 11) {
        const size_t healthyQueueTarget = targetQueue - 8;
        const size_t queueExcessThreshold = healthyQueueTarget + 3;
        const size_t rawQueueSize = queue_.size();
        if (rawQueueSize > queueExcessThreshold) {
          if (queueExcessSinceUs_ == 0) {
            queueExcessSinceUs_ = queueExcessCheckNowUs;
          } else if (queueExcessCheckNowUs - queueExcessSinceUs_ >=
                    kQueueExcessSustainedUs) {
            const size_t dropped = queue_.drainToNewest(healthyQueueTarget);
            if (dropped > 0) {
              sync_.onFramesWritten(dropped * snapclient::kFramesPerChunk);
              stats.chunksDropped += dropped;
              BELL_LOG(warn, logTag_,
                       "queue excess: raw queue {} -> {} (dropped {} "
                       "chunks, compensated {} frames)",
                       rawQueueSize, healthyQueueTarget, dropped,
                       dropped * snapclient::kFramesPerChunk);
            }
            queueExcessSinceUs_ = 0;
          }
        } else {
          queueExcessSinceUs_ = 0;
        }
      }
    } else {
      // Explicit reset - a resync's own drain already clears this anyway.
      queueExcessSinceUs_ = 0;
    }

    // playing_ went false -> true: evaluate() just reset
    // playbackStartTimeUs_/samplesWritten_ (see SyncEngine.cpp), from a
    // hard resync or a fresh connection.
    if (!wasPlayingBeforeEval && sync_.isPlaying()) {
      stats.lastResyncAtUs = nowUs();
    }

    stats.maybeLogSummary(playedLogLimiter_, logTag_, nowUs(), queueDepth,
                          i2sSink_.sendQueueOverflowCount());
  }

  size_t rawQueueSize() { return queue_.size(); }
  size_t pcmQueueSize() { return pcmQueue_.size(); }
  size_t decoderStackHighWaterMarkWords() const {
    return decoder_.stackHighWaterMarkWords();
  }

 private:
  static constexpr int64_t kBackgroundLogIntervalUs = 10'000'000;  // 10s
  // Longer than shortMedian_'s ~2s window, so one drain's outliers
  // clear it before the next drain's debounce could complete.
  static constexpr int64_t kQueueExcessSustainedUs = 3'000'000;  // 3s

  const char* logTag_;

  snapclient::SyncEngine sync_;
  snapclient::DspProcessor dsp_;
  snapclient::AudioSinkI2S i2sSink_;
  snapclient::BoundedQueue<snapclient::QueuedChunk> queue_;
  snapclient::BoundedQueue<snapclient::DecodedChunk> pcmQueue_;
  std::atomic<uint32_t> codecGeneration_{0};
  snapclient::DecoderTask decoder_;
  snapclient::SnapcastClient& client_;

  bell::audio::SampleRate sampleRate_ = bell::audio::SampleRate::SR_44100HZ;
  int32_t bufferMs_ = 0;
  // Snapcast's per-client "latency" setting, which nothing on this end
  // can measure directly.
  int32_t dacFixedLatencyMs_ = 0;
  // Tracks what SyncEngine was last told, so a ServerSettings message
  // that only changed volume/mute (bundled together in the same message
  // by the protocol) doesn't also force a resync - sync_.onSettingsChanged
  // drops playing_ back to the initial-sync state, which costs several
  // seconds to recover from and has nothing to do with volume.
  int32_t lastSyncBufferMs_ = 0;

  RateLimiter serverSettingsLogLimiter_;
  RateLimiter dropLogLimiter_;
  size_t queueFullDrops_ = 0;

  std::vector<int16_t> scratch_;
  std::vector<int16_t> scratchResampled_;
  int64_t queueExcessSinceUs_ = 0;
  RateLimiter playedLogLimiter_{kBackgroundLogIntervalUs};
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
      : bell::Task("snapclient", 8 * 1024, 5, bell::TaskCore::Core1,
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
    std::unique_ptr<PlaybackPipeline> pipeline;
    control.onSettingsChanged = [&] {
      pipeline->applyDspSettings(settings.activeFlow(),
                                 settings.flowParams(settings.activeFlow()));
      applyUdpLogSettings();
    };

    auto controlListenRes = control.listen(CONFIG_SNAPCLIENT_CONTROL_PORT);
    if (!controlListenRes) {
      BELL_LOG(error, kLogTag, "control server listen failed: {}",
               controlListenRes.error().message());
    }

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

    pipeline = std::make_unique<PlaybackPipeline>(client, kLogTag);
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
                              const std::byte* payload, size_t len,
                              int64_t serverTimeUs) {
      pipeline->onAudioChunk(codec, payload, len, serverTimeUs);
    };

    BELL_LOG(info, kLogTag, "connecting to {}:{}...", config.host,
             config.port);

    constexpr int64_t kBackgroundLogIntervalUs = 10'000'000;  // 10s
    RateLimiter queueLogLimiter(kBackgroundLogIntervalUs);
    RateLimiter stackLogLimiter(kBackgroundLogIntervalUs);
    PlaybackStats stats;

    while (true) {
      const int64_t logNow = nowUs();
      if (queueLogLimiter.due(logNow)) {
        // A growing gap between freeHeap and largestFreeBlock over time
        // is the fragmentation signal to watch for.
        BELL_LOG(info, kLogTag,
                 "queue depth={} pcmQueue depth={} freeHeap={} "
                 "largestFreeBlock={}",
                 pipeline->rawQueueSize(), pipeline->pcmQueueSize(),
                 esp_get_free_heap_size(),
                 snapclient::chunkHeapLargestFreeBlockBytes());
      }

      // Stack sizes below are all fixed guesses, not measured - this
      // reports real headroom so they can be right-sized instead of
      // guessed again.
      if (stackLogLimiter.due(logNow)) {
        BELL_LOG(info, kLogTag,
                 "stack headroom: snapclient_task={}B snapcast_client={}B "
                 "decoder_task={}B",
                 getStackHighWaterMarkWords() * sizeof(StackType_t),
                 client.stackHighWaterMarkWords() * sizeof(StackType_t),
                 pipeline->decoderStackHighWaterMarkWords() *
                     sizeof(StackType_t));
      }

      pipeline->consumeOnce(stats);
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
