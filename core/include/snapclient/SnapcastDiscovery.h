#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <bell/Result.h>
#include <bell/mdns/Browser.h>

namespace snapclient {

// Browses for Snapcast servers via mDNS (_snapcast._tcp). Decision-free:
// reports every server it manages to fully address-resolve, until stopped
// - picking one is the caller's job.
class SnapcastDiscovery {
 public:
  struct Found {
    std::string host;
    uint16_t port;
  };

  bell::Result<> start(std::function<void(const Found&)> onFound);
  void stop();

 private:
  // Outlives browser_, which calls it until destroyed.
  std::function<void(const Found&)> onFound_;
  std::unique_ptr<bell::mdns::Browser> browser_;
};

}  // namespace snapclient
