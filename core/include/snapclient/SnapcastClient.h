#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <bell/audio/Common.h>
#include <bell/audio/OpusCodec.h>
#include <bell/net/TCPSocket.h>
#include <bell/utils/Task.h>

#include "snapclient/Protocol.h"

namespace snapclient {

struct ServerSettings {
  int32_t bufferMs = 0;
  int32_t latencyMs = 0;
  uint32_t volume = 0;
  bool muted = false;
};

// Connects to a Snapcast server (static host:port - no mDNS discovery yet),
// does the HELLO handshake, and dispatches parsed messages via callbacks.
// Decodes Opus; PCM passes through unmodified; FLAC/OGG/unknown codecs
// disconnect (triggers a retry, same as any other connection error).
//
// Runs its own bell::Task; owns no playback/sync decisions - those live in
// SyncEngine, wired up by whoever constructs this.
class SnapcastClient : public bell::Task {
 public:
  struct Config {
    std::string host;
    uint16_t port = 1704;
    std::string clientName = "snapclient-cpp";
  };

  explicit SnapcastClient(Config config);
  ~SnapcastClient() override;

  // Fires after every successful connect (including reconnects) - the
  // server-clock relationship (TimeFilter's samples, drift estimate) from
  // any previous connection is no longer valid and should be reset.
  std::function<void()> onConnected;
  std::function<void(const bell::audio::Format&)> onCodecReady;
  std::function<void(const std::byte* pcm, size_t len, int64_t serverTimeUs)>
      onPcmChunk;
  std::function<void(const ServerSettings&)> onServerSettings;
  std::function<void(int64_t offsetUs, int64_t maxErrorUs, int64_t nowUs)>
      onTimeSample;

  // Caller (owner of the SyncEngine) switches this from the fast warm-up
  // cadence to a slower steady-state one once latency samples are enough.
  void setPingIntervalUs(int64_t intervalUs);

 protected:
  void taskLoop() override;
  void wakeTask() override;

 private:
  const char* LOG_TAG = "SnapcastClient";

  Config config_;
  bell::net::TCPSocket socket_;
  bool connected_ = false;
  uint16_t nextId_ = 0;
  int64_t lastTimeSyncSentUs_ = 0;
  int64_t pingIntervalUs_ = 10000;
  bool receivedCodecHeader_ = false;

  bell::audio::OpusCodec opusCodec_;
  bell::audio::Format pcmFormat_;
  Codec activeCodec_ = Codec::None;

  bool connectAndHandshake();
  bool readAndDispatchOne();
  bool readExact(std::byte* buf, size_t len);
  bool writeAll(const std::vector<std::byte>& data);
  void sendTimeSync();
  void disconnect();

  bool handleCodecHeader(const std::byte* payload, size_t len);
  void handleWireChunk(const std::byte* payload, size_t len);
  void handleServerSettings(const std::byte* payload, size_t len);
  void handleTime(const BaseMessage& base, const std::byte* payload,
                  size_t len);
};

}  // namespace snapclient
