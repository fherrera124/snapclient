#pragma once

#include <cstdint>
#include <functional>

#include <bell/Result.h>
#include <bell/http/Server.h>

#include "snapclient/ControlSettings.h"

namespace snapclient {

// GET/POST /api/settings over bell::http::Server. Decision-free: whoever
// constructs this wires onSettingsChanged to whatever a settings change
// should actually do (e.g. push new DSP params into a live DspProcessor).
class ControlServer {
 public:
  explicit ControlServer(ControlSettings& settings);

  bell::Result<> listen(uint16_t port = 8080);

  std::function<void()> onSettingsChanged;

 private:
  ControlSettings& settings_;
  bell::http::Server httpServer_;

  void registerRoutes();
};

}  // namespace snapclient
