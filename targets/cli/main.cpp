#include <bell/Logger.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "snapclient/Core.h"
#include "snapclient/DspProcessor.h"
#include "snapclient/SnapcastClient.h"
#include "snapclient/SyncEngine.h"

namespace {

int64_t nowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

struct QueuedChunk {
  int64_t serverTimeUs;
  std::vector<int16_t> pcm;
};

// Network receive (SnapcastClient's own task) and playback pacing (this
// loop) run on separate threads, connected by a queue, so evaluate()'s
// WaitMore decisions can re-check the same chunk against real elapsed time
// instead of a freshly-arrived one.
int runSnapcastTest(const std::string& host, uint16_t port) {
  const char* kLogTag = "snapcast_test";

  snapclient::SyncEngine sync;
  snapclient::DspProcessor dsp;
  bell::audio::SampleRate sampleRate = bell::audio::SampleRate::SR_44100HZ;
  int32_t bufferMs = 0;

  std::mutex queueMutex;
  std::deque<QueuedChunk> queue;

  size_t chunksPlayed = 0;
  size_t chunksDropped = 0;
  size_t corrections = 0;

  snapclient::SnapcastClient::Config config;
  config.host = host;
  config.port = port;
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

  BELL_LOG(info, kLogTag, "connecting to {}:{}...", host, port);

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
    return runSnapcastTest(argv[2], static_cast<uint16_t>(std::stoi(argv[3])));
  }

  return snapclient::dspSmokeTest() ? 0 : 1;
}
