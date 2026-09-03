#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace snapclient {

struct Timestamp {
  int32_t sec = 0;
  int32_t usec = 0;

  int64_t toMicros() const { return int64_t{sec} * 1000000 + usec; }
};

enum class MessageType : uint16_t {
  Base = 0,
  CodecHeader = 1,
  WireChunk = 2,
  ServerSettings = 3,
  Time = 4,
  Hello = 5,
};

struct BaseMessage {
  static constexpr size_t kWireSize = 26;

  MessageType type = MessageType::Base;
  uint16_t id = 0;
  uint16_t refersTo = 0;
  Timestamp sent;
  Timestamp received;
  uint32_t size = 0;

  std::vector<std::byte> serialize() const;
  static std::optional<BaseMessage> parse(const std::byte* data, size_t len);
};

struct HelloMessage {
  std::string mac;
  std::string hostname;
  std::string version;
  std::string clientName;
  std::string os;
  std::string arch;
  int instance = 1;
  std::string id;
  int protocolVersion = 2;

  std::vector<std::byte> serialize() const;
};

struct ServerSettingsMessage {
  int32_t bufferMs = 0;
  int32_t latency = 0;
  uint32_t volume = 0;
  bool muted = false;

  static std::optional<ServerSettingsMessage> parse(const std::byte* payload,
                                                     size_t len);
};

enum class Codec { None, Pcm, Opus, Flac };

struct CodecHeaderMessage {
  Codec codec = Codec::None;
  uint32_t sampleRate = 0;
  uint16_t bits = 0;
  uint16_t channels = 0;

  static std::optional<CodecHeaderMessage> parse(const std::byte* payload,
                                                  size_t len);
};

// The fixed-size header in front of a WireChunk message's audio payload -
// parsed on its own so the payload can be read straight into its final
// destination buffer instead of through an intermediate copy.
struct WireChunkHeader {
  static constexpr size_t kWireSize = 12;

  Timestamp timestamp;
  uint32_t payloadSize = 0;

  static std::optional<WireChunkHeader> parse(const std::byte* data,
                                               size_t len);
};

struct TimeMessage {
  Timestamp latency;

  std::vector<std::byte> serialize() const;
  static std::optional<TimeMessage> parse(const std::byte* payload,
                                          size_t len);
};

}  // namespace snapclient
