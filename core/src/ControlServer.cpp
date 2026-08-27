#include "snapclient/ControlServer.h"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include <bell/Logger.h>
#include <bell/http/Reader.h>
#include <bell/http/Writer.h>
#include <tao/json.hpp>

#include "snapclient/SettingsUiHtml.h"
#include "snapclient/SnapcastDiscovery.h"

#ifdef ESP_PLATFORM
#include <esp_system.h>
#endif

namespace snapclient {

namespace {
const char* kLogTag = "ControlServer";
}  // namespace

ControlServer::ControlServer(ControlSettings& settings)
    : settings_(settings) {
  registerRoutes();
}

bell::Result<> ControlServer::listen(uint16_t port) {
  return httpServer_.listen(port);
}

void ControlServer::registerRoutes() {
  auto registerHtmlPage = [this](const char* uri, const char* html) {
    httpServer_.registerGet(
        uri,
        [html](const std::unique_ptr<bell::http::Reader>& /*request*/,
              const std::unique_ptr<bell::http::Writer>& response,
              const std::unordered_map<std::string, std::string>& /*params*/) {
          (void)response->writeResponseWithBody(
              200, {{"Content-Type", "text/html; charset=utf-8"}}, html);
        });
  };
  httpServer_.registerGet(
      "/styles.css",
      [](const std::unique_ptr<bell::http::Reader>& /*request*/,
        const std::unique_ptr<bell::http::Writer>& response,
        const std::unordered_map<std::string, std::string>& /*params*/) {
        (void)response->writeResponseWithBody(
            200, {{"Content-Type", "text/css"}}, kSharedStyleCss);
      });
  registerHtmlPage("/", kNavShellHtml);
  registerHtmlPage("/general-settings.html", kGeneralSettingsHtml);
  registerHtmlPage("/dsp-settings.html", kDspSettingsHtml);
  registerHtmlPage("/dac-settings.html", kDacSettingsHtml);

  httpServer_.registerGet(
      "/api/settings",
      [this](const std::unique_ptr<bell::http::Reader>& /*request*/,
            const std::unique_ptr<bell::http::Writer>& response,
            const std::unordered_map<std::string, std::string>& /*params*/) {
        (void)response->writeResponseWithBody(
            200, {{"Content-Type", "application/json"}}, settings_.toJson());
      });

  httpServer_.registerGet(
      "/api/discover",
      [](const std::unique_ptr<bell::http::Reader>& /*request*/,
        const std::unique_ptr<bell::http::Writer>& response,
        const std::unordered_map<std::string, std::string>& /*params*/) {
        // Discovery stops via ~SnapcastDiscovery when this goes out of
        // scope below - no explicit stop() call needed for a one-shot browse.
        SnapcastDiscovery discovery;
        std::mutex mutex;
        std::condition_variable cv;
        std::optional<SnapcastDiscovery::Found> found;

        auto startRes = discovery.start(
            [&](const SnapcastDiscovery::Found& f) {
              std::lock_guard<std::mutex> lock(mutex);
              if (!found) {
                found = f;
                cv.notify_one();
              }
            });
        if (startRes) {
          std::unique_lock<std::mutex> lock(mutex);
          cv.wait_for(lock, std::chrono::milliseconds(4000),
                     [&] { return found.has_value(); });
        }

        if (!found) {
          (void)response->writeResponseWithBody(
              404, {{"Content-Type", "application/json"}},
              R"({"error":"no server found"})");
          return;
        }
        tao::json::value obj;
        obj["host"] = found->host;
        obj["port"] = found->port;
        (void)response->writeResponseWithBody(
            200, {{"Content-Type", "application/json"}},
            tao::json::to_string(obj));
      });

  httpServer_.registerPost(
      "/api/restart",
      [](const std::unique_ptr<bell::http::Reader>& /*request*/,
        const std::unique_ptr<bell::http::Writer>& response,
        const std::unordered_map<std::string, std::string>& /*params*/) {
#ifdef ESP_PLATFORM
        (void)response->writeResponseWithBody(
            200, {{"Content-Type", "application/json"}}, R"({"ok":true})");
        esp_restart();
#else
        BELL_LOG(warn, kLogTag, "restart requested, unsupported on this target");
        (void)response->writeResponseWithBody(
            501, {{"Content-Type", "application/json"}},
            R"({"error":"restart not supported on this target"})");
#endif
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
