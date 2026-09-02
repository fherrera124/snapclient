#pragma once

#include <cstdint>
#include <functional>

#include <bell/Result.h>
#include <bell/http/Server.h>

#include "snapclient/ControlSettings.h"
#include "snapclient/tas5805m/Tas5805mDriver.h"
#include "snapclient/tas5805m/Tas5805mSettings.h"

namespace snapclient {

// GET/POST /api/settings over bell::http::Server. Decision-free: whoever
// constructs this wires onSettingsChanged to whatever a settings change
// should actually do (e.g. push new DSP params into a live DspProcessor).
class ControlServer {
 public:
  explicit ControlServer(ControlSettings& settings);

  bell::Result<> listen(uint16_t port = 80);

  // Adds GET/POST /api/dac/settings and GET /api/dac/faults. Only call
  // this when a target actually has a TAS5805M wired - CLI runs and any
  // other target stay unaffected otherwise.
  void registerDacRoutes(Tas5805mSettings& dacSettings,
                         Tas5805mDriver& dacDriver);

  std::function<void()> onSettingsChanged;
  std::function<void()> onDacSettingsChanged;

 private:
  ControlSettings& settings_;
  bell::http::Server httpServer_;

  void registerRoutes();
};

}  // namespace snapclient
