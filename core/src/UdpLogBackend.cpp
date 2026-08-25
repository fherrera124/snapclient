#include "snapclient/UdpLogBackend.h"

#include <chrono>

#include <fmt/chrono.h>
#include <fmt/format.h>

namespace snapclient {

namespace {

char levelChar(bell::LogLevel level) {
  switch (level) {
    case bell::LogLevel::debug:
      return 'D';
    case bell::LogLevel::info:
      return 'I';
    case bell::LogLevel::warn:
      return 'W';
    case bell::LogLevel::error:
      return 'E';
  }
  return '?';
}

}  // namespace

bell::Result<std::unique_ptr<UdpLogBackend>> UdpLogBackend::create(
    const std::string& host, uint16_t port) {
  auto resolved = bell::IpAddress::resolveDomain(host, SOCK_DGRAM);
  if (!resolved) {
    return nonstd::make_unexpected(resolved.error());
  }
  resolved->setPort(port);

  bell::UDPSocket socket;
  auto fdRes = socket.createFd(resolved->getFamily());
  if (!fdRes) {
    return nonstd::make_unexpected(fdRes.error());
  }

  return std::unique_ptr<UdpLogBackend>(
      new UdpLogBackend(std::move(socket), *resolved));
}

UdpLogBackend::UdpLogBackend(bell::UDPSocket socket, bell::IpAddress address)
    : socket_(std::move(socket)), address_(address) {}

void UdpLogBackend::log(bell::LogLevel level, std::string_view filename,
                        int line, std::string_view tag,
                        std::string_view message) {
  auto now = std::chrono::system_clock::now();
  auto tNow = std::chrono::system_clock::to_time_t(now);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()) %
            1000;

  std::string formatted =
      tag.empty()
          ? fmt::format("[{:%H:%M:%S}.{:03}] {} {}:{}: {}\n",
                        fmt::localtime(tNow), ms.count(), levelChar(level),
                        filename, line, message)
          : fmt::format("[{:%H:%M:%S}.{:03}] {} [{}] {}:{}: {}\n",
                        fmt::localtime(tNow), ms.count(), levelChar(level),
                        tag, filename, line, message);

  (void)socket_.sendto(reinterpret_cast<const std::byte*>(formatted.data()),
                       formatted.size(), address_);
}

}  // namespace snapclient
