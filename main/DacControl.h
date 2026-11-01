#pragma once

#include <cstdint>
#include <vector>

#include <bell/dsp/BiquadParameters.h>

#include "driver/i2s_std.h"
#include "esp_err.h"

namespace snapclient {

// The DAC's control plane: bring-up, mute and, if it has one, volume.
// AudioSinkI2S owns the data plane and is the same for every DAC.
class DacControl {
 public:
  virtual ~DacControl() = default;

  // Called on every channel (re)creation, after the channel is initialized
  // and before it is enabled or primed.
  virtual esp_err_t start(i2s_chan_handle_t tx, uint32_t sampleRate) = 0;

  virtual void setMuted(bool muted) = 0;
  // Same contract as AudioSink::setVolume().
  virtual bool setVolume(float /*gain*/) { return false; }
  // Same contract as AudioSink::setEq().
  virtual bool setEq(
      const std::vector<bell::dsp::BiquadParameters>& /*stages*/) {
    return false;
  }
};

}  // namespace snapclient
