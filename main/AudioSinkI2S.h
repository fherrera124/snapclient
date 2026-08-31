#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "driver/gpio.h"
#include "driver/i2s_std.h"

namespace snapclient {

// Direct, synchronous ESP-IDF i2s_std sink for a PCM5102A-class DAC (pure
// I2S, no I2C control bus). No internal buffering or task: write() blocks
// on i2s_channel_write() on the caller's own thread, since SyncEngine's
// per-chunk pacing already happens there and must not be blurred by
// another buffering layer. 16-bit stereo only.
class AudioSinkI2S {
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

  // Tears down and recreates the I2S channel only if sampleRate differs
  // from the last call (or this is the first call).
  void configure(uint32_t sampleRate);

  void write(const std::byte* pcm, size_t len);

  // Time between write() and the DAC actually playing it, at the
  // last-configured sample rate. Assumes the caller paces write() calls
  // to real time rather than racing to fill the DMA ring - see
  // SyncEngine's WaitMore. 0 before the first configure() call.
  uint32_t outputBufferUs() const;

  // No-op if Config::mutePin is GPIO_NUM_NC.
  void setMuted(bool muted);

  // Cumulative count of DMA-underrun events (a zeroed buffer sent instead
  // of fresh data). ISR-incremented, safe to read from any task.
  uint32_t sendQueueOverflowCount() const;

  // sendQueueOverflowCount() in frames: each event is exactly one
  // descriptor's worth (kDmaFrameNum), since the driver fires it once per
  // completed descriptor while the queue stays starved.
  uint32_t underrunCompensationFrames() const;

 private:
  const char* LOG_TAG = "AudioSinkI2S";

  Config config_;
  i2s_chan_handle_t txChan_ = nullptr;
  uint32_t currentSampleRate_ = 0;
  // Frames written since the last full-descriptor boundary, mod one
  // descriptor's frame capacity - see outputBufferUs().
  uint32_t framesIntoCurrentDescriptor_ = 0;
  std::atomic<uint32_t> sendQueueOverflowCount_{0};

  void teardownChannel();
  void primeSilence();
  static bool onSendQueueOverflow(i2s_chan_handle_t handle,
                                  i2s_event_data_t* event, void* userCtx);
};

}  // namespace snapclient
