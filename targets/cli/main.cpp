#include <bell/Logger.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "snapclient/ControlServer.h"
#include "snapclient/ControlSettings.h"
#include "snapclient/Core.h"
#include "snapclient/DspProcessor.h"
#include "snapclient/JsonFileSettingsStore.h"
#include "snapclient/SnapcastClient.h"
#include "snapclient/SnapcastDiscovery.h"
#include "snapclient/SyncEngine.h"
#include "snapclient/UdpLogBackend.h"
#include "snapclient/tas5805m/Tas5805mDriver.h"
#include "snapclient/tas5805m/Tas5805mSettings.h"

namespace {

// No I2C bus exists on a host - lets /api/dac/settings and /api/dac/faults
// be exercised over HTTP without a real chip.
class NullI2cBus : public snapclient::I2cBus {
 public:
  bool write(uint8_t, const uint8_t*, size_t) override { return true; }
  bool writeThenRead(uint8_t, const uint8_t*, size_t, uint8_t* readBuf,
                     size_t readLen) override {
    std::fill(readBuf, readBuf + readLen, 0);
    return true;
  }
};

int64_t nowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

struct QueuedChunk {
  int64_t serverTimeUs;
  std::vector<int16_t> pcm;
};

std::optional<snapclient::SnapcastDiscovery::Found> discoverServer(
    int timeoutMs) {
  snapclient::SnapcastDiscovery discovery;
  std::mutex mutex;
  std::condition_variable cv;
  std::optional<snapclient::SnapcastDiscovery::Found> found;

  auto startRes = discovery.start(
      [&](const snapclient::SnapcastDiscovery::Found& f) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!found) {
          found = f;
          cv.notify_one();
        }
      });
  if (!startRes) {
    BELL_LOG(error, "snapcast_test", "mdns browse failed: {}",
             startRes.error().message());
    return std::nullopt;
  }

  std::unique_lock<std::mutex> lock(mutex);
  cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
             [&] { return found.has_value(); });
  return found;
}

// Network receive (SnapcastClient's own task) and playback pacing (this
// loop) run on separate threads, connected by a queue, so evaluate()'s
// WaitMore decisions can re-check the same chunk against real elapsed time
// instead of a freshly-arrived one.
int runSnapcastTest(const std::string& host, uint16_t port,
                    const std::string& settingsPath, uint16_t controlPort) {
  const char* kLogTag = "snapcast_test";

  snapclient::SyncEngine sync;
  snapclient::DspProcessor dsp;
  bell::audio::SampleRate sampleRate = bell::audio::SampleRate::SR_44100HZ;
  int32_t bufferMs = 0;

  snapclient::JsonFileSettingsStore settingsStore(settingsPath);
  snapclient::ControlSettings settings(settingsStore);
  snapclient::ControlServer control(settings);

  dsp.switchFlow(settings.activeFlow());
  dsp.setParams(settings.activeFlow(), settings.flowParams(settings.activeFlow()));

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
    BELL_LOG(info, kLogTag, "settings applied: flow={} freqPrimaryHz={}",
             static_cast<int>(settings.activeFlow()),
             settings.flowParams(settings.activeFlow()).freqPrimaryHz);
  };

  NullI2cBus dacBus;
  snapclient::Tas5805mDriver dacDriver(dacBus);
  snapclient::Tas5805mSettings dacSettings(settingsStore);
  control.registerDacRoutes(dacSettings, dacDriver);
  control.onDacSettingsChanged = [&] {
    BELL_LOG(info, kLogTag, "dac settings applied: analogGain={}",
             dacSettings.analogGain());
  };

  auto controlListenRes = control.listen(controlPort);
  if (!controlListenRes) {
    BELL_LOG(error, kLogTag, "control server listen failed: {}",
             controlListenRes.error().message());
  }

  std::mutex queueMutex;
  std::deque<QueuedChunk> queue;

  size_t chunksPlayed = 0;
  size_t chunksDropped = 0;
  size_t corrections = 0;

  snapclient::SnapcastClient::Config config;
  if (!settings.serverHost().empty()) {
    config.host = settings.serverHost();
    config.port = settings.serverPort();
    BELL_LOG(info, kLogTag, "using pinned server setting {}:{}", config.host,
             config.port);
  } else if (auto discovered = discoverServer(5000)) {
    config.host = discovered->host;
    config.port = discovered->port;
    BELL_LOG(info, kLogTag, "discovered server via mDNS: {}:{}", config.host,
             config.port);
  } else {
    config.host = host;
    config.port = port;
    BELL_LOG(info, kLogTag, "mDNS found nothing, falling back to {}:{}",
             config.host, config.port);
  }
  snapclient::SnapcastClient client(config);

  client.onServerSettings = [&](const snapclient::ServerSettings& s) {
    bufferMs = s.bufferMs;
    sync.onSettingsChanged(bufferMs, 0, static_cast<uint32_t>(sampleRate));
    BELL_LOG(info, kLogTag, "server settings: bufferMs={} volume={} muted={}",
             s.bufferMs, s.volume, s.muted);
  };

  client.onCodecReady = [&](const bell::audio::Format& fmt) {
    sampleRate = fmt.getSampleRate();
    sync.onSettingsChanged(bufferMs, 0, fmt.getSampleRateValue());
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

  BELL_LOG(info, kLogTag, "connecting to {}:{}...", config.host, config.port);

  std::vector<int16_t> scratch;
  const auto testEnd = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (std::chrono::steady_clock::now() < testEnd) {
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
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
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
        std::this_thread::sleep_for(std::chrono::microseconds(result.waitUs));
        continue;
      }
      if (result.decision == snapclient::PlayDecision::Play) {
        chunksPlayed++;
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
      BELL_LOG(info, kLogTag, "played={} dropped={} corrections={} queued={}",
               chunksPlayed, chunksDropped, corrections, queueDepth - 1);
    }
  }

  BELL_LOG(info, kLogTag, "final: played={} dropped={} corrections={}",
           chunksPlayed, chunksDropped, corrections);
  return chunksPlayed > 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  bell::registerDefaultLogger();
  snapclient::scaffoldSelfCheck();

  if (argc >= 4 && std::string(argv[1]) == "--snapcast") {
    std::string settingsPath = "/tmp/snapclient_settings.json";
    uint16_t controlPort = 8080;
    for (int i = 4; i + 1 < argc; i += 2) {
      std::string flag = argv[i];
      if (flag == "--settings") {
        settingsPath = argv[i + 1];
      } else if (flag == "--control-port") {
        controlPort = static_cast<uint16_t>(std::stoi(argv[i + 1]));
      }
    }
    return runSnapcastTest(argv[2], static_cast<uint16_t>(std::stoi(argv[3])),
                           settingsPath, controlPort);
  }

  bool ok = snapclient::dspSmokeTest();
  ok = snapclient::settingsSmokeTest() && ok;
  ok = snapclient::tas5805mDriverSmokeTest() && ok;
  ok = snapclient::tas5805mSettingsSmokeTest() && ok;
  ok = snapclient::udpLogBackendSmokeTest() && ok;
  return ok ? 0 : 1;
}
