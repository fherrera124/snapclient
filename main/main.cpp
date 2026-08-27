#include <bell/Logger.h>
#include <bell/utils/Semaphore.h>
#include <bell/utils/Task.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <tcb/span.hpp>

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

// Snapcast's Opus stream here always encodes 20ms frames at 48kHz -
// 960 samples per channel. Used to size the silence placeholder written
// in place of a chunk the pool couldn't hold, decodeOpus()'s output
// buffer, and the Pcm-codec pool slot size below (raw PCM has no
// compression to exploit, so a slot for it must fit a whole chunk).
constexpr size_t kFramesPerChunk = 960;
constexpr size_t kBytesPerFrame = 2 * sizeof(int16_t);  // stereo S16
constexpr size_t kPcmChunkBytes = kFramesPerChunk * kBytesPerFrame;

// Pre-allocated instead of per-chunk, to avoid heap fragmentation under
// sustained ~50 allocs/frees per second. PcmChunkPool prefers IRAM (a
// pool separate from the DRAM heap Opus/DSP/WiFi/HTTP draw from) but
// falls back to DRAM when IRAM has no room.
//
// Slots hold chunks as *received*, not decoded. For Opus, that means a
// raw encoded packet - capped at 1275 bytes by RFC 6716 for a 20ms frame
// regardless of bitrate, far smaller than the ~3.8KB its decoded PCM
// takes up - decodeOpus() only runs right before playback (see the
// consumer loop below), so the bulk of however many chunks the server's
// bufferMs asks for sits compactly encoded. Pcm has no such compression
// to exploit, so its slot must fit a whole raw chunk (kPcmChunkBytes) -
// sized separately below, and the pool is reconstructed for whichever
// codec is actually negotiated (see ensurePoolForCodec in runTask()),
// since committing to one slot size at compile time would either waste
// most of Opus's real buffering headroom or make Pcm permanently fall
// back to the pool-exhausted silence path.
constexpr size_t kOpusPoolSlotBytes = 1400;  // RFC 6716 max (1275) + margin
#if CONFIG_IDF_TARGET_ESP32
// Original ESP32, no PSRAM: ~320KB total SRAM has to cover WiFi, Opus,
// DSP and HTTP too - watch freeHeap at startup if raising this further,
// Opus decoder setup can fail under memory pressure on this board.
constexpr size_t kOpusPoolSlotCount = 32;  // ~44KB, ~640ms
// Same ~42KB this pool always held for Pcm, before tonight's Opus-only
// slot shrink - Pcm was never affected by that, so it keeps its
// original capacity rather than gaining or losing anything.
constexpr size_t kPcmPoolSlotCount = 11;  // ~220ms
#else
// Same ~44KB/~640ms Opus budget as the ESP32 branch above, scaled up -
// not independently tuned against this (PSRAM) board's real headroom.
constexpr size_t kOpusPoolSlotCount = 44;  // ~880ms
constexpr size_t kPcmPoolSlotCount = 15;  // ~300ms
#endif

// Bounds the network-to-playback queue independently of the pool sizes -
// those are sized against the memory-tight pool of real payload buffers,
// but a queued item without one still carries useful timing (it falls
// back to the pool-exhausted silence path in onAudioChunk below), so
// starving the queue itself to match the pool's memory budget only turns
// bursts into unaccounted drops instead of accounted silence. Set above
// what the Opus pool (kOpusPoolSlotCount, the common case) can back with
// real data, not tied to any one server-configured bufferMs.
constexpr size_t kQueueCapacity = 40;

int64_t nowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

struct QueuedChunk {
  int64_t serverTimeUs;
  snapclient::Codec codec = snapclient::Codec::None;
  // Encoded (Opus) or raw (Pcm codec) payload, exactly as received -
  // decodeOpus() runs later, in the consumer loop, not here.
  snapclient::PcmChunkPool::PooledBuffer payload;
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
  // Sized against a measured high-water mark (~5.5KB) plus margin, not a
  // round-number guess - see the periodic stack headroom log below.
  SnapclientTask()
      : bell::Task("snapclient", 16 * 1024, 5, bell::TaskCore::Core1,
                   /*espStackOnPsram=*/false) {
    startTask();
  }

  void runTask() override {
    const char* kLogTag = "snapclient_task";

    snapclient::SyncEngine sync;
    snapclient::DspProcessor dsp;
    bell::audio::SampleRate sampleRate = bell::audio::SampleRate::SR_44100HZ;
    int32_t bufferMs = 0;
    int32_t serverLatencyMs = 0;
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

    // Reconstructed by ensurePoolForCodec() below for whichever codec is
    // actually negotiated - null only before the first one ever is.
    std::unique_ptr<snapclient::PcmChunkPool> pcmPool;
    snapclient::Codec pcmPoolCodec = snapclient::Codec::None;
    auto ensurePoolForCodec = [&](snapclient::Codec codec) {
      if (codec == pcmPoolCodec) {
        return;
      }
      pcmPoolCodec = codec;
      // release(), not reset(): a PooledBuffer the consumer thread
      // already checked out from the old pool before this runs (not
      // ruled out - this fires on the network thread, independently of
      // the consumer's own loop timing) still needs its original pool
      // object to exist when it releases back. Actual codec transitions
      // are rare - a reconnect with the same codec never reaches here,
      // since pcmPoolCodec already matches - so deliberately leaking one
      // pool's worth of storage on the rare transition that does happen
      // beats synchronizing a safe hand-off between threads for
      // something that may never occur in a stable deployment.
      (void)pcmPool.release();
      if (codec == snapclient::Codec::Pcm) {
        pcmPool = std::make_unique<snapclient::PcmChunkPool>(kPcmPoolSlotCount,
                                                             kPcmChunkBytes);
      } else {
        pcmPool = std::make_unique<snapclient::PcmChunkPool>(
            kOpusPoolSlotCount, kOpusPoolSlotBytes);
      }
    };
    ensurePoolForCodec(snapclient::Codec::Opus);
    std::mutex queueMutex;
    std::deque<QueuedChunk> queue;
    // Lets the empty-queue wait below react as soon as onAudioChunk pushes
    // something, instead of polling on a fixed interval.
    bell::Semaphore chunkAvailable;

    size_t chunksPlayed = 0;
    size_t chunksDropped = 0;
    size_t corrections = 0;
    size_t silencePlayed = 0;

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
      serverLatencyMs = s.latencyMs;
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

    client.onCodecReady = [&](snapclient::Codec codec,
                              const bell::audio::Format& fmt) {
      sampleRate = fmt.getSampleRate();
      i2sSink.configure(fmt.getSampleRateValue());
      lastSyncBufferMs = bufferMs;
      sync.onSettingsChanged(bufferMs, fmt.getSampleRateValue());
      {
        // Queued chunks predate this codec header and would otherwise
        // decode against the *new* decoder instance setupDecode() just
        // recreated in SnapcastClient.
        std::lock_guard<std::mutex> lock(queueMutex);
        queue.clear();
      }
      ensurePoolForCodec(codec);
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
      std::lock_guard<std::mutex> lock(queueMutex);
      if (queue.size() >= kQueueCapacity) {
        queueFullDrops++;
      } else {
        QueuedChunk item;
        item.serverTimeUs = serverTimeUs;
        item.codec = codec;
        // Pool exhaustion queues a silent placeholder (item.payload left
        // empty) instead of dropping the chunk outright - a dropped
        // chunk leaves a gap in the played frame count that the
        // server's clock doesn't have. See silencePlayed below for how
        // often this actually results in audible silence.
        item.payload = pcmPool->acquire(payload, len);
        // chunkAvailable is an uncapped counting semaphore; the consumer
        // only take()s it when idle, not once per dequeue - giving on
        // every push would grow the count unbounded, and take() then
        // never actually waits.
        const bool wasEmpty = queue.empty();
        queue.push_back(std::move(item));
        if (wasEmpty) {
          chunkAvailable.give();
        }
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

    BELL_LOG(info, kLogTag, "connecting to {}:{}...", config.host,
             config.port);

    std::vector<int16_t> scratch;
    const std::vector<int16_t> silenceInput(kFramesPerChunk * 2, 0);
    // Heap, not stack (like scratch above) - this task's stack is sized
    // tight against a measured high-water mark, and a buffer this size
    // is enough to overflow it.
    std::vector<std::byte> decodeBuf(kPcmChunkBytes);
    int64_t lastQueueLogUs = 0;
    int64_t lastTimingLogUs = 0;
    int64_t lastDropYieldUs = 0;
    int64_t lastStackLogUs = 0;
    int64_t lastSilenceLogUs = 0;
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

      // Stack sizes below (SnapclientTask, SnapcastClient) are both fixed
      // guesses, not measured - this reports real headroom so they can be
      // right-sized instead of guessed again.
      if (logNow - lastStackLogUs >= 10000000) {
        lastStackLogUs = logNow;
        BELL_LOG(info, kLogTag,
                 "stack headroom: snapclient_task={}B snapcast_client={}B",
                 getStackHighWaterMarkWords() * sizeof(StackType_t),
                 client.stackHighWaterMarkWords() * sizeof(StackType_t));
      }

      // Aggregated, not logged per-occurrence at either substitution site
      // below (pool exhausted, opus decode failed) - the same sustained-
      // overload/UART-stall reasoning as the queue-full log above.
      if (logNow - lastSilenceLogUs >= 30000000) {
        lastSilenceLogUs = logNow;
        if (silencePlayed > 0) {
          BELL_LOG(warn, kLogTag,
                   "played {} chunks as silence (pool exhausted or opus "
                   "decode failed) in the last 30s",
                   silencePlayed);
          silencePlayed = 0;
        }
      }

      if (queueDepth == 0) {
        // Woken as soon as onAudioChunk pushes something - the 10ms
        // timeout is only a fallback. Below CONFIG_FREERTOS_HZ=100's
        // 10ms tick, vTaskDelay rounds to 0 and just yields once, not
        // enough for CPU1's idle task to feed the watchdog under
        // sustained load.
        chunkAvailable.take(10);
        continue;
      }

      const std::byte* dspInput;
      size_t dspInputLen;
      size_t frames;
      if (!item.payload) {
        // Pool-exhausted placeholder - no payload was ever stored for
        // this chunk, encoded or not.
        dspInput = reinterpret_cast<const std::byte*>(silenceInput.data());
        dspInputLen = silenceInput.size() * sizeof(int16_t);
        frames = kFramesPerChunk;
        silencePlayed++;
      } else if (item.codec == snapclient::Codec::Opus) {
        auto decoded = client.decodeOpus(
            tcb::span<const std::byte>(item.payload.data(),
                                       item.payload.size()),
            decodeBuf.data(), decodeBuf.size());
        if (!decoded) {
          dspInput = reinterpret_cast<const std::byte*>(silenceInput.data());
          dspInputLen = silenceInput.size() * sizeof(int16_t);
          frames = kFramesPerChunk;
          silencePlayed++;
        } else {
          dspInput = decodeBuf.data();
          dspInputLen = *decoded;
          frames = dspInputLen / kBytesPerFrame;
        }
      } else {
        dspInput = item.payload.data();
        dspInputLen = item.payload.size();
        frames = dspInputLen / kBytesPerFrame;
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

      // Recomputed fresh per chunk, not settings-cached: outputBufferUs()
      // reflects where the previous write() actually landed in the DMA
      // ring, not a fixed assumption about it.
      const int32_t dacLatencyUs =
          static_cast<int32_t>(serverLatencyMs) * 1000 +
          static_cast<int32_t>(i2sSink.outputBufferUs());

      for (;;) {
        auto result = sync.evaluate(item.serverTimeUs, nowUs(), queueDepth,
                                    dacLatencyUs);
        if (result.decision == snapclient::PlayDecision::WaitMore) {
          std::this_thread::sleep_for(
              std::chrono::microseconds(result.waitUs));
          continue;
        }
        if (result.decision == snapclient::PlayDecision::Play) {
          chunksPlayed++;

          const auto* writeStart =
              reinterpret_cast<const std::byte*>(scratch.data());
          size_t writeLen = scratch.size() * sizeof(int16_t);

          // Realizes frameAdjustment on the actual I2S bytes: negative
          // drops that many frames from this write (catch up), positive
          // writes them again after (slow down). Magnitude scales with
          // how far off sync is, not fixed to one frame - see
          // SyncEngine.cpp.
          const size_t adjustFrames =
              static_cast<size_t>(result.frameAdjustment < 0
                                      ? -result.frameAdjustment
                                      : result.frameAdjustment);
          const size_t adjustBytes = adjustFrames * kBytesPerFrame;
          if (result.frameAdjustment < 0 && writeLen >= adjustBytes) {
            writeLen -= adjustBytes;
          }
          const int64_t i2sStartUs = nowUs();
          i2sSink.write(writeStart, writeLen);
          if (result.frameAdjustment > 0 && writeLen >= adjustBytes) {
            i2sSink.write(writeStart + writeLen - adjustBytes, adjustBytes);
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
          // Unlike Play (blocks on I2S write) or WaitMore (sleeps), this
          // path doesn't yield - sustained drops would spin this core hot
          // enough to starve the network task, delaying chunks further
          // and keeping every next one too late to recover. Throttled,
          // not per-drop, so draining a short burst isn't itself
          // rate-limited below real-time.
          const int64_t yieldNow = nowUs();
          if (yieldNow - lastDropYieldUs >= 20000) {
            lastDropYieldUs = yieldNow;
            // take(), not sleep_for(): returns as soon as a new chunk
            // arrives instead of always waiting the full timeout.
            chunkAvailable.take(10);
          }

          if (!sync.isPlaying()) {
            // Still resyncing after this drop - the rest of the backlog
            // only gets staler evaluated one at a time in FIFO order,
            // since real time keeps advancing while working through it.
            // Jump straight to the newest queued chunk instead.
            std::lock_guard<std::mutex> lock(queueMutex);
            while (queue.size() > 1) {
              queue.pop_front();
              chunksDropped++;
            }
          }
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
