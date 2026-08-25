#include "snapclient/ControlServer.h"

#include <memory>
#include <string>
#include <unordered_map>

#include <bell/http/Reader.h>
#include <bell/http/Writer.h>
#include <tao/json.hpp>

#include "snapclient/SettingsUiHtml.h"

namespace snapclient {

ControlServer::ControlServer(ControlSettings& settings)
    : settings_(settings) {
  registerRoutes();
}

bell::Result<> ControlServer::listen(uint16_t port) {
  return httpServer_.listen(port);
}

void ControlServer::registerRoutes() {
  httpServer_.registerGet(
      "/",
      [](const std::unique_ptr<bell::http::Reader>& /*request*/,
        const std::unique_ptr<bell::http::Writer>& response,
        const std::unordered_map<std::string, std::string>& /*params*/) {
        (void)response->writeResponseWithBody(
            200, {{"Content-Type", "text/html; charset=utf-8"}},
            kSettingsUiHtml);
      });

  httpServer_.registerGet(
      "/api/settings",
      [this](const std::unique_ptr<bell::http::Reader>& /*request*/,
            const std::unique_ptr<bell::http::Writer>& response,
            const std::unordered_map<std::string, std::string>& /*params*/) {
        (void)response->writeResponseWithBody(
            200, {{"Content-Type", "application/json"}}, settings_.toJson());
      });

  httpServer_.registerPost(
      "/api/settings",
      [this](const std::unique_ptr<bell::http::Reader>& request,
            const std::unique_ptr<bell::http::Writer>& response,
            const std::unordered_map<std::string, std::string>& /*params*/) {
        auto bodyRes = request->getBodyStringView();
        if (!bodyRes || !settings_.applyJson(std::string(*bodyRes))) {
          (void)response->writeResponseWithBody(
              400, {{"Content-Type", "application/json"}},
              R"({"error":"invalid settings payload"})");
          return;
        }
        if (onSettingsChanged) {
          onSettingsChanged();
        }
        (void)response->writeResponseWithBody(
            200, {{"Content-Type", "application/json"}}, settings_.toJson());
      });
}

void ControlServer::registerDacRoutes(Tas5805mSettings& dacSettings,
                                      Tas5805mDriver& dacDriver) {
  httpServer_.registerGet(
      "/api/dac/settings",
      [&dacSettings](
          const std::unique_ptr<bell::http::Reader>& /*request*/,
          const std::unique_ptr<bell::http::Writer>& response,
          const std::unordered_map<std::string, std::string>& /*params*/) {
        (void)response->writeResponseWithBody(
            200, {{"Content-Type", "application/json"}},
            dacSettings.toJson());
      });

  httpServer_.registerPost(
      "/api/dac/settings",
      [this, &dacSettings](
          const std::unique_ptr<bell::http::Reader>& request,
          const std::unique_ptr<bell::http::Writer>& response,
          const std::unordered_map<std::string, std::string>& /*params*/) {
        auto bodyRes = request->getBodyStringView();
        if (!bodyRes || !dacSettings.applyJson(std::string(*bodyRes))) {
          (void)response->writeResponseWithBody(
              400, {{"Content-Type", "application/json"}},
              R"({"error":"invalid settings payload"})");
          return;
        }
        if (onDacSettingsChanged) {
          onDacSettingsChanged();
        }
        (void)response->writeResponseWithBody(
            200, {{"Content-Type", "application/json"}},
            dacSettings.toJson());
      });

  httpServer_.registerGet(
      "/api/dac/faults",
      [&dacDriver](
          const std::unique_ptr<bell::http::Reader>& /*request*/,
          const std::unique_ptr<bell::http::Writer>& response,
          const std::unordered_map<std::string, std::string>& /*params*/) {
        Tas5805mFaults faults;
        if (!dacDriver.getFaults(faults)) {
          (void)response->writeResponseWithBody(
              502, {{"Content-Type", "application/json"}},
              R"({"error":"failed to read faults"})");
          return;
        }
        tao::json::value obj;
        obj["rightOverCurrent"] = faults.rightOverCurrent;
        obj["leftOverCurrent"] = faults.leftOverCurrent;
        obj["rightDcFault"] = faults.rightDcFault;
        obj["leftDcFault"] = faults.leftDcFault;
        obj["pvddUnderVoltage"] = faults.pvddUnderVoltage;
        obj["pvddOverVoltage"] = faults.pvddOverVoltage;
        obj["clockFault"] = faults.clockFault;
        obj["biquadWriteFailed"] = faults.biquadWriteFailed;
        obj["otpCrcError"] = faults.otpCrcError;
        obj["overTemperatureShutdown"] = faults.overTemperatureShutdown;
        obj["overTemperatureWarning"] = faults.overTemperatureWarning;
        (void)response->writeResponseWithBody(
            200, {{"Content-Type", "application/json"}},
            tao::json::to_string(obj));
      });
}

}  // namespace snapclient
