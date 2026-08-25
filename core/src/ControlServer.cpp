#include "snapclient/ControlServer.h"

#include <memory>
#include <string>
#include <unordered_map>

#include <bell/http/Reader.h>
#include <bell/http/Writer.h>

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

}  // namespace snapclient
