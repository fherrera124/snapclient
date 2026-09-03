#pragma once

#include <cstddef>
#include <cstdint>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "snapclient/AudioSink.h"

namespace snapclient {

// Direct, synchronous ESP-IDF i2s_std sink for a PCM5102A-class DAC (pure
// I2S, no I2C control bus). No internal buffering or task: write() blocks
// on i2s_channel_write() on the caller's own thread, since SyncEngine's
// per-chunk pacing already happens there and must not be blurred by
// another buffering layer. 16-bit stereo only.
class AudioSinkI2S : public snapclient::AudioSink {
 public:
  struct Config {
    int port = I2S_NUM_0;
    gpio_num_t bclkPin = GPIO_NUM_NC;
    gpio_num_t wsPin = GPIO_NUM_NC;
    gpio_num_t doutPin = GPIO_NUM_NC;
    gpio_num_t mclkPin = I2S_GPIO_UNUSED;
    // GPIO_NUM_NC disables mute handling entirely (e.g. XSMT tied via an
    // onboard pull-up, not software-controlled).
    gpio_num_t mutePin = GPIO_NUM_NC;
  };

  explicit AudioSinkI2S(Config config);
  ~AudioSinkI2S();

  AudioSinkI2S(const AudioSinkI2S&) = delete;
  AudioSinkI2S& operator=(const AudioSinkI2S&) = delete;

  // Tears down and recreates the I2S channel only if sampleRate or
  // chunkFrames differs from the last call (or this is the first call).
  void configure(uint32_t sampleRate, uint32_t chunkFrames) override;

  void write(const std::byte* pcm, size_t len) override;

  // No-op if Config::mutePin is GPIO_NUM_NC.
  void setMuted(bool muted) override;

  size_t preload(const std::byte* pcm, size_t len) override;
  void disable() override;
  void enable() override;

 private:
  const char* LOG_TAG = "AudioSinkI2S";

  Config config_;
  i2s_chan_handle_t txChan_ = nullptr;
  uint32_t currentSampleRate_ = 0;
  uint32_t currentChunkFrames_ = 0;

  void teardownChannel();
  void primeSilence();
};

}  // namespace snapclient
