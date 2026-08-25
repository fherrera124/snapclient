#include "snapclient/SnapcastDiscovery.h"

#include <bell/mdns/Manager.h>

namespace snapclient {

bell::Result<> SnapcastDiscovery::start(
    std::function<void(const Found&)> onFound) {
  onFound_ = std::move(onFound);

  auto res = bell::mdns::getDefaultManager()->browse(
      "_snapcast._tcp", "", 0,
      [this](const bell::MDNSDiscoveryEvent& ev) {
        if (ev.service.port == 0 || !ev.service.ipv4) {
          return;
        }
        auto addr = ev.service.ipv4->toString(false);
        if (!addr) {
          return;
        }
        std::string host = *addr;
        if (onFound_) {
          onFound_({host, ev.service.port});
        }
      },
      /*autoResolveService=*/true, /*autoResolveAddresses=*/true,
      // IPv6 link-local addresses need a scope id to be connectable, which
      // ServiceRecord doesn't carry - request IPv4-only resolution.
      /*resolveIPv6=*/false);
  if (!res) {
    return nonstd::make_unexpected(res.error());
  }
  browser_ = std::move(*res);
  return {};
}

void SnapcastDiscovery::stop() {
  browser_.reset();
}

}  // namespace snapclient
