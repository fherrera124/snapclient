#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#include "snapclient/AudioSink.h"
#include "snapclient/ChunkBuffer.h"
#include "snapclient/PlaybackPipeline.h"
#include "snapclient/PrecisionWaiter.h"
#include "snapclient/SnapcastClient.h"

namespace {

int gFailures = 0;

void checkImpl(bool cond, const char* expr, const char* file, int line) {
  if (!cond) {
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
    gFailures++;
  }
}

}  // namespace

#define CHECK(cond) checkImpl((cond), #cond, __FILE__, __LINE__)

namespace snapclient {
namespace {

using std::chrono::microseconds;
using std::chrono::milliseconds;

int64_t nowUs() {
  return std::chrono::duration_cast<microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Paces writes like a DMA ring, and records whether its channel was ever
// driven from more than one thread.
class FakeSink : public AudioSink {
 public:
  static constexpr uint32_t kRingFrames = 2048;

  void configure(uint32_t sampleRate, uint32_t /*chunkFrames*/) override {
    noteChannelThread();
    sampleRate_ = sampleRate != 0 ? sampleRate : 44100;
  }

  void write(const std::byte* /*pcm*/, size_t len) override {
    noteChannelThread();
    const size_t frames = len / kSinkBytesPerFrame;
    framesWritten_ += frames;
    std::this_thread::sleep_for(microseconds(frames * 1'000'000 / sampleRate_));
  }

  size_t preload(const std::byte* /*pcm*/, size_t len) override {
    noteChannelThread();
    const size_t room = (kRingFrames - preloadedFrames_) * kSinkBytesPerFrame;
    const size_t accepted = std::min(len, room);
    preloadedFrames_ += accepted / kSinkBytesPerFrame;
    return accepted;
  }

  void disable() override {
    noteChannelThread();
    preloadedFrames_ = 0;
  }

  void enable() override { noteChannelThread(); }
  void setMuted(bool /*muted*/) override {}
  uint32_t ringCapacityFrames() const override { return kRingFrames; }

  bool drivenFromTwoThreads() const { return twoThreads_; }
  size_t framesWritten() const { return framesWritten_; }

 private:
  void noteChannelThread() {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::thread::id self = std::this_thread::get_id();
    if (owner_ == std::thread::id{}) {
      owner_ = self;
    } else if (owner_ != self) {
      twoThreads_ = true;
    }
  }

  std::mutex mutex_;
  std::thread::id owner_;
  std::atomic<bool> twoThreads_{false};
  std::atomic<size_t> framesWritten_{0};
  uint32_t sampleRate_ = 44100;
  size_t preloadedFrames_ = 0;
};

void sendTimeSamples(PlaybackPipeline& pipeline, int count) {
  for (int i = 0; i < count; ++i) {
    // TimeFilter ignores a sample no newer than the last one.
    std::this_thread::sleep_for(milliseconds(1));
    pipeline.onTimeSample(/*offsetUs=*/0, /*maxErrorUs=*/100, nowUs());
  }
}

// Plays SnapcastClient's thread: a real-time PCM stream with a latency
// change, a format change and a reconnect along the way.
void feed(PlaybackPipeline& pipeline) {
  ServerSettings settings;
  settings.bufferMs = 300;
  settings.volume = 50;
  uint32_t sampleRate = 44100;

  pipeline.onCodecReady(Codec::Pcm,
                        bell::audio::Format(2, bell::audio::SampleFormat::S16,
                                            bell::audio::SampleRate::SR_44100HZ));
  pipeline.onServerSettings(settings);
  sendTimeSamples(pipeline, 30);

  const int64_t startUs = nowUs();
  int64_t nextChunkUs = startUs;
  for (int step = 0;; ++step) {
    const int64_t elapsedUs = nowUs() - startUs;
    if (elapsedUs > 3'000'000) {
      break;
    }
    if (step == 60) {
      settings.latencyMs = 1;
      pipeline.onServerSettings(settings);
    }
    if (step == 90) {
      sampleRate = 48000;
      pipeline.onCodecReady(
          Codec::Pcm, bell::audio::Format(2, bell::audio::SampleFormat::S16,
                                          bell::audio::SampleRate::SR_48000HZ));
    }
    if (step == 110) {
      pipeline.onConnected();
      sendTimeSamples(pipeline, 30);
    }
    if (step % 50 == 0) {
      sendTimeSamples(pipeline, 1);
    }

    const size_t frames = sampleRate / 50;  // 20 ms
    std::vector<std::byte> silence(frames * 4);
    pipeline.onAudioChunk(Codec::Pcm,
                          acquireChunkBuffer(silence.data(), silence.size()),
                          nowUs());
    nextChunkUs += 20'000;
    std::this_thread::sleep_for(microseconds(nextChunkUs - nowUs()));
  }
}

void testOutputIsDrivenFromOneThread() {
  SnapcastClient::Config config;
  config.host = "127.0.0.1";
  config.port = 9;
  // Never destroyed: DecoderTask has no way to stop.
  auto* client = new SnapcastClient(config);
  auto* sink = new FakeSink();
  auto* waiter = new SleepPrecisionWaiter();
  auto* pipeline = new PlaybackPipeline(*client, *sink, *waiter, "test");

  std::atomic<bool> stop{false};
  std::thread consumer([&] {
    while (!stop) {
      pipeline->consumeOnce();
    }
  });

  feed(*pipeline);
  const size_t framesBeforeEnd = sink->framesWritten();
  stop = true;
  consumer.join();

  CHECK(!sink->drivenFromTwoThreads());
  CHECK(framesBeforeEnd > 44100);
}

}  // namespace
}  // namespace snapclient

int main() {
  snapclient::testOutputIsDrivenFromOneThread();

  if (gFailures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", gFailures);
    std::fflush(stderr);
    std::_Exit(1);
  }
  std::printf("all tests passed\n");
  std::fflush(stdout);
  std::_Exit(0);
}
