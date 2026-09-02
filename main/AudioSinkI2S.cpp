#include "AudioSinkI2S.h"

#include <algorithm>
#include <array>

#include "bell/Logger.h"
#include "freertos/FreeRTOS.h"

namespace snapclient {

namespace {
constexpr uint32_t kPrimeSilenceMs = 100;
constexpr size_t kBytesPerFrame = 2 /*channels*/ * sizeof(int16_t);
constexpr size_t kSilenceChunkFrames = 256;
constexpr size_t kDmaDescNum = 4;
constexpr size_t kDmaFrameNum = 960;
}  // namespace

AudioSinkI2S::AudioSinkI2S(Config config) : config_(config) {
  if (config_.mutePin != GPIO_NUM_NC) {
    gpio_config_t muteConf = {
        .pin_bit_mask = 1ULL << config_.mutePin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&muteConf);
  }
  setMuted(true);
}

AudioSinkI2S::~AudioSinkI2S() {
  teardownChannel();
}

void AudioSinkI2S::teardownChannel() {
  if (txChan_ == nullptr) {
    return;
  }
  i2s_channel_disable(txChan_);
  i2s_del_channel(txChan_);
  txChan_ = nullptr;
}

void AudioSinkI2S::configure(uint32_t sampleRate) {
  if (txChan_ != nullptr && sampleRate == currentSampleRate_) {
    return;
  }

  setMuted(true);
  teardownChannel();

  i2s_chan_config_t chanConfig =
      I2S_CHANNEL_DEFAULT_CONFIG(config_.port, I2S_ROLE_MASTER);
  // i2s_channel_write() blocks until the DMA queue has room - kDmaDescNum
  // sets how much hardware buffering absorbs write() scheduling jitter
  // before that blocking becomes audible as a stall.
  chanConfig.dma_desc_num = kDmaDescNum;
  chanConfig.dma_frame_num = kDmaFrameNum;
  chanConfig.auto_clear = true;
  esp_err_t err = i2s_new_channel(&chanConfig, &txChan_, nullptr);
  if (err != ESP_OK) {
    BELL_LOG(error, LOG_TAG, "i2s_new_channel failed: {}",
             static_cast<int>(err));
    txChan_ = nullptr;
    return;
  }

  i2s_std_config_t stdConfig = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sampleRate),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = config_.mclkPin,
              .bclk = config_.bclkPin,
              .ws = config_.wsPin,
              .dout = config_.doutPin,
              .din = I2S_GPIO_UNUSED,
              .invert_flags = {.mclk_inv = false,
                               .bclk_inv = false,
                               .ws_inv = false},
          },
  };

  // Force the high-precision Audio PLL clock source
  stdConfig.clk_cfg.clk_src = I2S_CLK_SRC_APLL;

  err = i2s_channel_init_std_mode(txChan_, &stdConfig);
  if (err != ESP_OK) {
    BELL_LOG(error, LOG_TAG, "i2s_channel_init_std_mode failed: {}",
             static_cast<int>(err));
    teardownChannel();
    return;
  }

  err = i2s_channel_enable(txChan_);
  if (err != ESP_OK) {
    BELL_LOG(error, LOG_TAG, "i2s_channel_enable failed: {}",
             static_cast<int>(err));
    teardownChannel();
    return;
  }

  currentSampleRate_ = sampleRate;
  primeSilence();
  setMuted(false);
}

void AudioSinkI2S::primeSilence() {
  // Written in small fixed-size chunks off the stack, not one big heap
  // buffer - this runs before playback starts, when a single large
  // contiguous allocation is least likely to find room next to whatever
  // else (WiFi, HTTP, the PCM chunk pool) has already claimed heap space.
  std::array<std::byte, kSilenceChunkFrames * kBytesPerFrame> silenceChunk{};
  size_t framesRemaining = currentSampleRate_ * kPrimeSilenceMs / 1000;
  while (framesRemaining > 0) {
    const size_t framesThisWrite =
        std::min(framesRemaining, kSilenceChunkFrames);
    write(silenceChunk.data(), framesThisWrite * kBytesPerFrame);
    framesRemaining -= framesThisWrite;
  }
}

void AudioSinkI2S::write(const std::byte* pcm, size_t len) {
  if (txChan_ == nullptr) {
    return;
  }
  size_t written = 0;
  while (written < len) {
    size_t chunkWritten = 0;
    esp_err_t err = i2s_channel_write(txChan_, pcm + written, len - written,
                                      &chunkWritten, portMAX_DELAY);
    if (err != ESP_OK) {
      BELL_LOG(warn, LOG_TAG, "i2s_channel_write failed: {}",
               static_cast<int>(err));
      break;
    }
    written += chunkWritten;
  }
}

void AudioSinkI2S::setMuted(bool muted) {
  if (config_.mutePin == GPIO_NUM_NC) {
    return;
  }
  gpio_set_level(config_.mutePin, muted ? 0 : 1);
}

size_t AudioSinkI2S::preload(const std::byte* pcm, size_t len) {
  if (txChan_ == nullptr) {
    return 0;
  }
  size_t bytesLoaded = 0;
  esp_err_t err = i2s_channel_preload_data(txChan_, pcm, len, &bytesLoaded);
  if (err != ESP_OK) {
    BELL_LOG(warn, LOG_TAG, "i2s_channel_preload_data failed: {}",
             static_cast<int>(err));
    return 0;
  }
  return bytesLoaded;
}

void AudioSinkI2S::disable() {
  if (txChan_ != nullptr) {
    i2s_channel_disable(txChan_);
  }
}

void AudioSinkI2S::enable() {
  if (txChan_ != nullptr) {
    i2s_channel_enable(txChan_);
  }
}

}  // namespace snapclient
