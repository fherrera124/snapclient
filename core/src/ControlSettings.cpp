#include "snapclient/ControlSettings.h"

#include <array>
#include <cstddef>

#include <tao/json.hpp>

namespace snapclient {

namespace {
constexpr const char* kFlowNames[] = {"stereo", "biamp", "bassBoost",
                                      "eqBassTreble"};

const char* flowName(DspFlow flow) {
  return kFlowNames[static_cast<size_t>(flow)];
}

std::optional<DspFlow> flowFromName(const std::string& name) {
  for (size_t i = 0; i < std::size(kFlowNames); i++) {
    if (name == kFlowNames[i]) {
      return static_cast<DspFlow>(i);
    }
  }
  return std::nullopt;
}

const char* kServerHostKey = "server.host";
const char* kServerPortKey = "server.port";
const char* kHostnameKey = "device.hostname";
const char* kActiveFlowKey = "dsp.activeFlow";
const char* kUdpLogEnabledKey = "logging.enabled";
const char* kUdpLogHostKey = "logging.udpHost";
const char* kUdpLogPortKey = "logging.udpPort";

std::string paramKey(DspFlow flow, const char* field) {
  return std::string("dsp.") + flowName(flow) + "." + field;
}
}  // namespace

ControlSettings::ControlSettings(SettingsStore& store) : store_(store) {
  load();
}

void ControlSettings::load() {
  if (auto v = store_.getString(kServerHostKey)) {
    serverHost_ = *v;
  }
  if (auto v = store_.getInt(kServerPortKey)) {
    serverPort_ = static_cast<uint16_t>(*v);
  }
  if (auto v = store_.getString(kHostnameKey)) {
    hostname_ = *v;
  }
  if (auto v = store_.getString(kActiveFlowKey)) {
    if (auto flow = flowFromName(*v)) {
      activeFlow_ = *flow;
    }
  }
  for (size_t i = 0; i < flowParams_.size(); i++) {
    auto flow = static_cast<DspFlow>(i);
    DspFilterParams params;
    if (auto v = store_.getFloat(paramKey(flow, "freqPrimaryHz"))) {
      params.freqPrimaryHz = *v;
    }
    if (auto v = store_.getFloat(paramKey(flow, "gainPrimaryDb"))) {
      params.gainPrimaryDb = *v;
    }
    if (auto v = store_.getFloat(paramKey(flow, "freqTertiaryHz"))) {
      params.freqTertiaryHz = *v;
    }
    if (auto v = store_.getFloat(paramKey(flow, "gainTertiaryDb"))) {
      params.gainTertiaryDb = *v;
    }
    flowParams_[i] = params;
  }
  if (auto v = store_.getInt(kUdpLogEnabledKey)) {
    udpLogEnabled_ = (*v != 0);
  }
  if (auto v = store_.getString(kUdpLogHostKey)) {
    udpLogHost_ = *v;
  }
  if (auto v = store_.getInt(kUdpLogPortKey)) {
    udpLogPort_ = static_cast<uint16_t>(*v);
  }
}

std::string ControlSettings::serverHost() const {
  std::scoped_lock lock(mutex_);
  return serverHost_;
}

void ControlSettings::setServerHost(const std::string& host) {
  std::scoped_lock lock(mutex_);
  serverHost_ = host;
  store_.setString(kServerHostKey, host);
}

uint16_t ControlSettings::serverPort() const {
  std::scoped_lock lock(mutex_);
  return serverPort_;
}

void ControlSettings::setServerPort(uint16_t port) {
  std::scoped_lock lock(mutex_);
  serverPort_ = port;
  store_.setInt(kServerPortKey, port);
}

std::string ControlSettings::hostname() const {
  std::scoped_lock lock(mutex_);
  return hostname_;
}

void ControlSettings::setHostname(const std::string& hostname) {
  std::scoped_lock lock(mutex_);
  hostname_ = hostname;
  store_.setString(kHostnameKey, hostname);
}

DspFlow ControlSettings::activeFlow() const {
  std::scoped_lock lock(mutex_);
  return activeFlow_;
}

void ControlSettings::setActiveFlow(DspFlow flow) {
  std::scoped_lock lock(mutex_);
  activeFlow_ = flow;
  store_.setString(kActiveFlowKey, flowName(flow));
}

DspFilterParams ControlSettings::flowParams(DspFlow flow) const {
  std::scoped_lock lock(mutex_);
  return flowParams_[static_cast<size_t>(flow)];
}

void ControlSettings::setFlowParams(DspFlow flow,
                                    const DspFilterParams& params) {
  std::scoped_lock lock(mutex_);
  flowParams_[static_cast<size_t>(flow)] = params;
  store_.setFloat(paramKey(flow, "freqPrimaryHz"), params.freqPrimaryHz);
  store_.setFloat(paramKey(flow, "gainPrimaryDb"), params.gainPrimaryDb);
  store_.setFloat(paramKey(flow, "freqTertiaryHz"), params.freqTertiaryHz);
  store_.setFloat(paramKey(flow, "gainTertiaryDb"), params.gainTertiaryDb);
}

bool ControlSettings::udpLogEnabled() const {
  std::scoped_lock lock(mutex_);
  return udpLogEnabled_;
}

std::string ControlSettings::udpLogHost() const {
  std::scoped_lock lock(mutex_);
  return udpLogHost_;
}

uint16_t ControlSettings::udpLogPort() const {
  std::scoped_lock lock(mutex_);
  return udpLogPort_;
}

void ControlSettings::setUdpLog(bool enabled, const std::string& host,
                                uint16_t port) {
  std::scoped_lock lock(mutex_);
  udpLogEnabled_ = enabled;
  udpLogHost_ = host;
  udpLogPort_ = port;
  store_.setInt(kUdpLogEnabledKey, enabled ? 1 : 0);
  store_.setString(kUdpLogHostKey, host);
  store_.setInt(kUdpLogPortKey, port);
}

std::string ControlSettings::toJson() const {
  std::scoped_lock lock(mutex_);

  tao::json::value flows;
  for (size_t i = 0; i < flowParams_.size(); i++) {
    const auto& p = flowParams_[i];
    tao::json::value f;
    f["freqPrimaryHz"] = p.freqPrimaryHz;
    f["gainPrimaryDb"] = p.gainPrimaryDb;
    f["freqTertiaryHz"] = p.freqTertiaryHz;
    f["gainTertiaryDb"] = p.gainTertiaryDb;
    flows[kFlowNames[i]] = f;
  }

  tao::json::value obj;
  obj["server"]["host"] = serverHost_;
  obj["server"]["port"] = serverPort_;
  obj["device"]["hostname"] = hostname_;
  obj["dsp"]["activeFlow"] = flowName(activeFlow_);
  obj["dsp"]["flows"] = flows;

  obj["logging"]["enabled"] = udpLogEnabled_;
  obj["logging"]["udpHost"] = udpLogHost_;
  obj["logging"]["udpPort"] = udpLogPort_;

  return tao::json::to_string(obj);
}

bool ControlSettings::applyJson(const std::string& json) {
  tao::json::value obj;
  try {
    obj = tao::json::from_string(json);
  } catch (const std::exception&) {
    return false;
  }
  if (!obj.is_object()) {
    return false;
  }

  std::optional<std::string> newHost;
  std::optional<uint16_t> newPort;
  std::optional<std::string> newHostname;
  std::optional<DspFlow> newActiveFlow;
  std::array<std::optional<DspFilterParams>, 4> newFlowParams;
  std::optional<bool> newUdpLogEnabled;
  std::optional<std::string> newUdpLogHost;
  std::optional<uint16_t> newUdpLogPort;

  try {
    if (const auto* server = obj.find("server")) {
      if (!server->is_object()) {
        return false;
      }
      if (const auto* host = server->find("host")) {
        newHost = host->as<std::string>();
      }
      if (const auto* port = server->find("port")) {
        newPort = static_cast<uint16_t>(port->as<uint32_t>());
      }
    }
    if (const auto* device = obj.find("device")) {
      if (!device->is_object()) {
        return false;
      }
      if (const auto* hostname = device->find("hostname")) {
        newHostname = hostname->as<std::string>();
      }
    }
    if (const auto* dsp = obj.find("dsp")) {
      if (!dsp->is_object()) {
        return false;
      }
      if (const auto* active = dsp->find("activeFlow")) {
        auto flow = flowFromName(active->as<std::string>());
        if (!flow) {
          return false;
        }
        newActiveFlow = *flow;
      }
      if (const auto* flowsObj = dsp->find("flows")) {
        if (!flowsObj->is_object()) {
          return false;
        }
        for (const auto& [name, value] : flowsObj->get_object()) {
          auto flow = flowFromName(name);
          if (!flow) {
            return false;
          }
          if (!value.is_object()) {
            return false;
          }
          DspFilterParams params = flowParams(*flow);
          if (const auto* v = value.find("freqPrimaryHz")) {
            params.freqPrimaryHz = v->as<float>();
          }
          if (const auto* v = value.find("gainPrimaryDb")) {
            params.gainPrimaryDb = v->as<float>();
          }
          if (const auto* v = value.find("freqTertiaryHz")) {
            params.freqTertiaryHz = v->as<float>();
          }
          if (const auto* v = value.find("gainTertiaryDb")) {
            params.gainTertiaryDb = v->as<float>();
          }
          newFlowParams[static_cast<size_t>(*flow)] = params;
        }
      }
    }
    if (const auto* logging = obj.find("logging")) {
      if (!logging->is_object()) {
        return false;
      }
      bool enabled = udpLogEnabled();
      std::string host = udpLogHost();
      uint16_t port = udpLogPort();
      if (const auto* v = logging->find("enabled")) {
        enabled = v->as<bool>();
      }
      if (const auto* v = logging->find("udpHost")) {
        host = v->as<std::string>();
      }
      if (const auto* v = logging->find("udpPort")) {
        port = static_cast<uint16_t>(v->as<uint32_t>());
      }
      newUdpLogEnabled = enabled;
      newUdpLogHost = host;
      newUdpLogPort = port;
    }
  } catch (const std::exception&) {
    return false;
  }

  if (newHost) {
    setServerHost(*newHost);
  }
  if (newPort) {
    setServerPort(*newPort);
  }
  if (newHostname) {
    setHostname(*newHostname);
  }
  if (newActiveFlow) {
    setActiveFlow(*newActiveFlow);
  }
  for (size_t i = 0; i < newFlowParams.size(); i++) {
    if (newFlowParams[i]) {
      setFlowParams(static_cast<DspFlow>(i), *newFlowParams[i]);
    }
  }
  if (newUdpLogEnabled || newUdpLogHost || newUdpLogPort) {
    setUdpLog(newUdpLogEnabled.value_or(udpLogEnabled()),
             newUdpLogHost.value_or(udpLogHost()),
             newUdpLogPort.value_or(udpLogPort()));
  }
  return true;
}

}  // namespace snapclient
