#include "snapclient/Protocol.h"

#include <tao/json.hpp>

namespace snapclient {

namespace {
uint16_t readU16LE(const std::byte* p) {
  return static_cast<uint16_t>(std::to_integer<uint8_t>(p[0])) |
         (static_cast<uint16_t>(std::to_integer<uint8_t>(p[1])) << 8);
}

uint32_t readU32LE(const std::byte* p) {
  return static_cast<uint32_t>(std::to_integer<uint8_t>(p[0])) |
         (static_cast<uint32_t>(std::to_integer<uint8_t>(p[1])) << 8) |
         (static_cast<uint32_t>(std::to_integer<uint8_t>(p[2])) << 16) |
         (static_cast<uint32_t>(std::to_integer<uint8_t>(p[3])) << 24);
}

int32_t readI32LE(const std::byte* p) {
  return static_cast<int32_t>(readU32LE(p));
}

void writeU16LE(std::vector<std::byte>& out, uint16_t v) {
  out.push_back(std::byte(v & 0xFF));
  out.push_back(std::byte((v >> 8) & 0xFF));
}

void writeU32LE(std::vector<std::byte>& out, uint32_t v) {
  out.push_back(std::byte(v & 0xFF));
  out.push_back(std::byte((v >> 8) & 0xFF));
  out.push_back(std::byte((v >> 16) & 0xFF));
  out.push_back(std::byte((v >> 24) & 0xFF));
}

void writeI32LE(std::vector<std::byte>& out, int32_t v) {
  writeU32LE(out, static_cast<uint32_t>(v));
}

void appendString(std::vector<std::byte>& out, const std::string& s) {
  const auto* bytes = reinterpret_cast<const std::byte*>(s.data());
  out.insert(out.end(), bytes, bytes + s.size());
}
}  // namespace

std::vector<std::byte> BaseMessage::serialize() const {
  std::vector<std::byte> out;
  out.reserve(kWireSize);
  writeU16LE(out, static_cast<uint16_t>(type));
  writeU16LE(out, id);
  writeU16LE(out, refersTo);
  writeI32LE(out, sent.sec);
  writeI32LE(out, sent.usec);
  writeI32LE(out, received.sec);
  writeI32LE(out, received.usec);
  writeU32LE(out, size);
  return out;
}

std::optional<BaseMessage> BaseMessage::parse(const std::byte* data,
                                              size_t len) {
  if (len < kWireSize) {
    return std::nullopt;
  }
  BaseMessage m;
  m.type = static_cast<MessageType>(readU16LE(data));
  m.id = readU16LE(data + 2);
  m.refersTo = readU16LE(data + 4);
  m.sent.sec = readI32LE(data + 6);
  m.sent.usec = readI32LE(data + 10);
  m.received.sec = readI32LE(data + 14);
  m.received.usec = readI32LE(data + 18);
  m.size = readU32LE(data + 22);
  return m;
}

std::vector<std::byte> HelloMessage::serialize() const {
  tao::json::value obj;
  obj["MAC"] = mac;
  obj["HostName"] = hostname;
  obj["Version"] = version;
  obj["ClientName"] = clientName;
  obj["OS"] = os;
  obj["Arch"] = arch;
  obj["Instance"] = instance;
  obj["ID"] = id;
  obj["SnapStreamProtocolVersion"] = protocolVersion;

  std::string json = tao::json::to_string(obj);
  std::vector<std::byte> out;
  out.reserve(4 + json.size());
  writeU32LE(out, static_cast<uint32_t>(json.size()));
  appendString(out, json);
  return out;
}

std::optional<ServerSettingsMessage> ServerSettingsMessage::parse(
    const std::byte* payload, size_t len) {
  if (len < 4) {
    return std::nullopt;
  }
  uint32_t jsonLen = readU32LE(payload);
  if (len < size_t{4} + jsonLen) {
    return std::nullopt;
  }
  std::string json(reinterpret_cast<const char*>(payload + 4), jsonLen);

  tao::json::value obj;
  try {
    obj = tao::json::from_string(json);
  } catch (const std::exception&) {
    return std::nullopt;
  }

  ServerSettingsMessage m;
  try {
    m.bufferMs = obj.at("bufferMs").as<int32_t>();
    m.latency = obj.at("latency").as<int32_t>();
    m.volume = obj.at("volume").as<uint32_t>();
    m.muted = obj.at("muted").as<bool>();
  } catch (const std::exception&) {
    return std::nullopt;
  }
  return m;
}

std::optional<CodecHeaderMessage> CodecHeaderMessage::parse(
    const std::byte* payload, size_t len) {
  if (len < 4) {
    return std::nullopt;
  }
  uint32_t nameLen = readU32LE(payload);
  if (len < size_t{4} + nameLen + 4) {
    return std::nullopt;
  }
  std::string name(reinterpret_cast<const char*>(payload + 4), nameLen);

  size_t offset = 4 + nameLen;
  uint32_t payloadLen = readU32LE(payload + offset);
  offset += 4;
  if (len < offset + payloadLen) {
    return std::nullopt;
  }
  const std::byte* codecPayload = payload + offset;

  CodecHeaderMessage m;
  if (name == "opus") {
    m.codec = Codec::Opus;
    if (payloadLen >= 12) {
      m.sampleRate = readU32LE(codecPayload + 4);
      m.bits = readU16LE(codecPayload + 8);
      m.channels = readU16LE(codecPayload + 10);
    }
  } else if (name == "pcm") {
    m.codec = Codec::Pcm;
    if (payloadLen >= 36) {
      m.channels = readU16LE(codecPayload + 22);
      m.sampleRate = readU32LE(codecPayload + 24);
      m.bits = readU16LE(codecPayload + 34);
    }
  } else if (name == "flac") {
    m.codec = Codec::Flac;
  } else {
    m.codec = Codec::None;
  }
  return m;
}

std::optional<WireChunkHeader> WireChunkHeader::parse(const std::byte* data,
                                                       size_t len) {
  if (len < kWireSize) {
    return std::nullopt;
  }
  WireChunkHeader h;
  h.timestamp.sec = readI32LE(data);
  h.timestamp.usec = readI32LE(data + 4);
  h.payloadSize = readU32LE(data + 8);
  return h;
}

std::vector<std::byte> TimeMessage::serialize() const {
  std::vector<std::byte> out;
  out.reserve(8);
  writeI32LE(out, latency.sec);
  writeI32LE(out, latency.usec);
  return out;
}

std::optional<TimeMessage> TimeMessage::parse(const std::byte* payload,
                                              size_t len) {
  if (len < 8) {
    return std::nullopt;
  }
  TimeMessage m;
  m.latency.sec = readI32LE(payload);
  m.latency.usec = readI32LE(payload + 4);
  return m;
}

}  // namespace snapclient
