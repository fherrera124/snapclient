#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <bell/Logger.h>
#include <bell/Result.h>
#include <bell/net/IpAddress.h>
#include <bell/net/UDPSocket.h>

namespace snapclient {

// Sends every log call as one plain-text UDP packet, formatted for
// netcat/syslog-style viewers (no ANSI colors), with no buffering or
// retry.
class UdpLogBackend : public bell::LoggerBackend {
 public:
  static bell::Result<std::unique_ptr<UdpLogBackend>> create(
      const std::string& host, uint16_t port);

  void log(bell::LogLevel level, std::string_view filename, int line,
          std::string_view tag, std::string_view message) override;

 private:
  UdpLogBackend(bell::UDPSocket socket, bell::IpAddress address);

  bell::UDPSocket socket_;
  bell::IpAddress address_;
};

}  // namespace snapclient
