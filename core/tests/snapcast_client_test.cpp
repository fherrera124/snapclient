#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "snapclient/Protocol.h"
#include "snapclient/SnapcastClient.h"

namespace {

int gFailures = 0;

void checkImpl(bool cond, const char* expr, const char* file, int line) {
  if (!cond) {
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
    gFailures++;
  }
}

}  // namespace

#define CHECK(cond) checkImpl((cond), #cond, __FILE__, __LINE__)

namespace snapclient {
namespace {

using std::chrono::milliseconds;

class Bytes {
 public:
  Bytes& u16(uint16_t v) {
    for (int i = 0; i < 2; ++i) {
      data_.push_back(std::byte((v >> (8 * i)) & 0xFF));
    }
    return *this;
  }

  Bytes& u32(uint32_t v) {
    for (int i = 0; i < 4; ++i) {
      data_.push_back(std::byte((v >> (8 * i)) & 0xFF));
    }
    return *this;
  }

  Bytes& text(const std::string& s) {
    for (char c : s) {
      data_.push_back(std::byte(static_cast<uint8_t>(c)));
    }
    return *this;
  }

  Bytes& append(const Bytes& other) {
    data_.insert(data_.end(), other.data_.begin(), other.data_.end());
    return *this;
  }

  const std::vector<std::byte>& data() const { return data_; }

 private:
  std::vector<std::byte> data_;
};

// A snapserver that answers the hello at once with settings and a PCM codec
// header, as the real one does.
class FakeServer {
 public:
  FakeServer() {
    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::listen(listenFd_, 1);
    socklen_t len = sizeof(addr);
    ::getsockname(listenFd_, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);
    thread_ = std::thread([this] { serve(); });
  }

  ~FakeServer() {
    stop_ = true;
    thread_.join();
    ::close(listenFd_);
  }

  uint16_t port() const { return port_; }
  bool accepted() const { return accepted_; }

 private:
  static void send(int fd, MessageType type, const Bytes& payload) {
    BaseMessage base;
    base.type = type;
    base.size = static_cast<uint32_t>(payload.data().size());
    const auto header = base.serialize();
    ::send(fd, header.data(), header.size(), MSG_NOSIGNAL);
    ::send(fd, payload.data().data(), payload.data().size(), MSG_NOSIGNAL);
  }

  bool readable(int fd) const {
    while (!stop_) {
      pollfd pfd{fd, POLLIN, 0};
      if (::poll(&pfd, 1, 10) > 0) {
        return true;
      }
    }
    return false;
  }

  void serve() {
    if (!readable(listenFd_)) {
      return;
    }
    const int fd = ::accept(listenFd_, nullptr, nullptr);
    accepted_ = true;

    std::vector<std::byte> header(BaseMessage::kWireSize);
    if (readable(fd) &&
        ::recv(fd, header.data(), header.size(), MSG_WAITALL) ==
            static_cast<ssize_t>(header.size())) {
      const auto hello = BaseMessage::parse(header.data(), header.size());
      std::vector<std::byte> body(hello ? hello->size : 0);
      ::recv(fd, body.data(), body.size(), MSG_WAITALL);

      const std::string json =
          R"({"bufferMs":500,"latency":0,"muted":false,"volume":50})";
      send(fd, MessageType::ServerSettings,
           Bytes().u32(static_cast<uint32_t>(json.size())).text(json));

      Bytes wav;
      wav.text("RIFF").u32(36).text("WAVE").text("fmt ").u32(16);
      wav.u16(1).u16(2).u32(44100).u32(44100 * 4).u16(4).u16(16);
      wav.text("data").u32(0);
      send(fd, MessageType::CodecHeader,
           Bytes().u32(3).text("pcm").u32(44).append(wav));
    }

    while (!stop_) {
      std::this_thread::sleep_for(milliseconds(10));
    }
    ::close(fd);
  }

  int listenFd_ = -1;
  uint16_t port_ = 0;
  std::atomic<bool> accepted_{false};
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

template <typename Pred>
bool waitFor(Pred pred, int timeoutMs) {
  const auto deadline =
      std::chrono::steady_clock::now() + milliseconds(timeoutMs);
  while (!pred()) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    std::this_thread::sleep_for(milliseconds(5));
  }
  return true;
}

void testFirstMessagesReachCallbacksSetBeforeStart() {
  FakeServer server;
  SnapcastClient::Config config;
  config.host = "127.0.0.1";
  config.port = server.port();
  // Never destroyed: stopping closes its socket from this thread.
  auto* client = new SnapcastClient(config);

  // main.cpp builds the DAC and the pipeline in between.
  std::this_thread::sleep_for(milliseconds(100));
  CHECK(!server.accepted());

  std::atomic<int> settingsCount{0};
  std::atomic<int32_t> bufferMs{0};
  std::atomic<int> codecCount{0};
  std::atomic<uint32_t> sampleRate{0};
  client->onServerSettings = [&](const ServerSettings& s) {
    bufferMs = s.bufferMs;
    ++settingsCount;
  };
  client->onCodecReady = [&](Codec codec, const bell::audio::Format& fmt) {
    if (codec == Codec::Pcm) {
      sampleRate = fmt.getSampleRateValue();
    }
    ++codecCount;
  };
  client->start();

  CHECK(waitFor([&] { return codecCount > 0; }, 2000));
  CHECK(settingsCount == 1);
  CHECK(bufferMs == 500);
  CHECK(codecCount == 1);
  CHECK(sampleRate == 44100);
}

}  // namespace
}  // namespace snapclient

int main() {
  snapclient::testFirstMessagesReachCallbacksSetBeforeStart();

  if (gFailures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", gFailures);
    std::fflush(stderr);
    std::_Exit(1);
  }
  std::printf("all tests passed\n");
  std::fflush(stdout);
  std::_Exit(0);
}
