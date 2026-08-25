#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <string>

#include "snapclient/DspProcessor.h"
#include "snapclient/SettingsStore.h"

namespace snapclient {

// Typed settings model over a SettingsStore. Mutations persist immediately
// and update the in-memory cache; nothing here talks to a running
// DspProcessor or SnapcastClient - the caller wires that up.
class ControlSettings {
 public:
  explicit ControlSettings(SettingsStore& store);

  std::string serverHost() const;
  void setServerHost(const std::string& host);

  uint16_t serverPort() const;
  void setServerPort(uint16_t port);

  DspFlow activeFlow() const;
  void setActiveFlow(DspFlow flow);

  DspFilterParams flowParams(DspFlow flow) const;
  void setFlowParams(DspFlow flow, const DspFilterParams& params);

  bool udpLogEnabled() const;
  std::string udpLogHost() const;
  uint16_t udpLogPort() const;
  void setUdpLog(bool enabled, const std::string& host, uint16_t port);

  std::string toJson() const;

  // Validates the whole partial payload before applying anything: false
  // (no changes made) on a parse error, wrong field type, or unknown flow
  // name.
  bool applyJson(const std::string& json);

 private:
  SettingsStore& store_;
  mutable std::mutex mutex_;

  std::string serverHost_;
  uint16_t serverPort_ = 1704;
  DspFlow activeFlow_ = DspFlow::Stereo;
  std::array<DspFilterParams, 4> flowParams_{};
  bool udpLogEnabled_ = false;
  std::string udpLogHost_;
  uint16_t udpLogPort_ = 9999;

  void load();
};

}  // namespace snapclient
