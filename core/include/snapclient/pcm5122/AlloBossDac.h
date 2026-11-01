#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

#include <bell/dsp/BiquadParameters.h>

#include "snapclient/pcm5122/Pcm5122Driver.h"
#include "snapclient/pcm5122/Pcm5122Eq.h"

namespace snapclient {

// Allo Boss: a PCM5122 driving I2S from one of two onboard oscillators,
// switched through its own GPIOs. Follows Linux's allo-boss-dac.c.
// Thread-safe.
class AlloBossDac {
 public:
  explicit AlloBossDac(Pcm5122Driver& dac);

  bool reset();

  // Leaves the chip in standby on any failure, so it never drives clocks it
  // was not configured for. False also when the board gives no SCK, i.e. it
  // cannot be the master.
  bool start(uint32_t sampleRate);

  bool setMuted(bool muted);
  bool setVolumeCode(uint8_t code);

  // Both oscillators off; BCK and LRCK come from the host.
  bool startSlave();

  // Filters both channels in the chip, kept across start(). True when the
  // chip runs exactly these stages; otherwise it runs none, as it does
  // until start(), unless a failed write left it running others, which is
  // logged and retried on the next call.
  bool setEq(std::vector<bell::dsp::BiquadParameters> stages);

 private:
  std::optional<Pcm5122Eq> wantedEq() const;
  // In standby. False when a write failed.
  bool loadEq(const std::optional<Pcm5122Eq>& eq);

  Pcm5122Driver& dac_;
  std::mutex mutex_;
  std::vector<bell::dsp::BiquadParameters> eqStages_;
  // 0 while the chip is not running as master.
  uint32_t sampleRate_ = 0;
  bool imageLoaded_ = false;
  // What the chip runs, if eqKnown_; nullopt for no EQ.
  std::optional<Pcm5122Eq> loadedEq_;
  bool eqKnown_ = false;
};

}  // namespace snapclient
