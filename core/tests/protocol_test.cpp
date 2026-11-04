#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include <tao/json.hpp>

#include "snapclient/Protocol.h"

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

  // Snapcast's string: a 32-bit length, then the bytes.
  Bytes& sized(const std::string& s) {
    u32(static_cast<uint32_t>(s.size()));
    return text(s);
  }

  Bytes& append(const Bytes& other) {
    data_.insert(data_.end(), other.data_.begin(), other.data_.end());
    return *this;
  }

  const std::byte* data() const { return data_.data(); }
  size_t size() const { return data_.size(); }

 private:
  std::vector<std::byte> data_;
};

Bytes codecHeader(const std::string& name, const Bytes& codecPayload) {
  Bytes out;
  out.sized(name).u32(static_cast<uint32_t>(codecPayload.size()));
  return out.append(codecPayload);
}

std::optional<ServerSettingsMessage> parseSettings(const std::string& json) {
  Bytes payload;
  payload.sized(json);
  return ServerSettingsMessage::parse(payload.data(), payload.size());
}

void testBaseMessageWireLayout() {
  BaseMessage m;
  m.type = MessageType::Time;
  m.id = 0x0102;
  m.refersTo = 0x0304;
  m.sent = {-5, 999'999};
  m.received = {7, 12};
  m.size = 0x0A0B0C0D;

  const auto bytes = m.serialize();
  CHECK(bytes.size() == BaseMessage::kWireSize);
  CHECK(bytes[0] == std::byte{4});
  CHECK(bytes[1] == std::byte{0});
  CHECK(bytes[22] == std::byte{0x0D});
  CHECK(bytes[25] == std::byte{0x0A});

  const auto parsed = BaseMessage::parse(bytes.data(), bytes.size());
  CHECK(parsed.has_value());
  if (parsed) {
    CHECK(parsed->type == MessageType::Time);
    CHECK(parsed->id == 0x0102);
    CHECK(parsed->refersTo == 0x0304);
    CHECK(parsed->sent.sec == -5 && parsed->sent.usec == 999'999);
    CHECK(parsed->received.sec == 7 && parsed->received.usec == 12);
    CHECK(parsed->size == 0x0A0B0C0D);
  }
  CHECK(!BaseMessage::parse(bytes.data(), bytes.size() - 1));
}

void testServerSettingsFromSnapserver() {
  const auto m = parseSettings(
      R"({"bufferMs":1000,"latency":20,"muted":true,"volume":75})");
  CHECK(m.has_value());
  if (m) {
    CHECK(m->bufferMs == 1000);
    CHECK(m->latency == 20);
    CHECK(m->muted);
    CHECK(m->volume == 75);
  }
}

void testServerSettingsRejectsBadJson() {
  CHECK(!parseSettings(R"({"bufferMs":1000,"latency":20)"));
  CHECK(!parseSettings(R"({"bufferMs":1000,"latency":20,"volume":75})"));
  CHECK(!parseSettings(
      R"({"bufferMs":1000,"latency":20,"muted":true,"volume":"75"})"));
}

void testServerSettingsClampsVolume() {
  const auto low = parseSettings(
      R"({"bufferMs":1000,"latency":20,"muted":false,"volume":-1})");
  CHECK(low.has_value() && low->volume == 0);
  const auto high = parseSettings(
      R"({"bufferMs":1000,"latency":20,"muted":false,"volume":150})");
  CHECK(high.has_value() && high->volume == 100);
}

void testServerSettingsRejectsBadLengths() {
  const std::string json =
      R"({"bufferMs":1000,"latency":20,"muted":true,"volume":75})";
  for (uint32_t declared :
       {static_cast<uint32_t>(json.size() + 1), 0xFFFFFFFCu, 0xFFFFFFFFu}) {
    Bytes payload;
    payload.u32(declared).text(json);
    CHECK(!ServerSettingsMessage::parse(payload.data(), payload.size()));
  }
  Bytes tooShort;
  tooShort.u16(0).text("{");
  CHECK(!ServerSettingsMessage::parse(tooShort.data(), tooShort.size()));
}

void testCodecHeaderOpus() {
  Bytes opus;
  opus.text("OPUS").u32(48000).u16(16).u16(2);
  const Bytes payload = codecHeader("opus", opus);
  const auto m = CodecHeaderMessage::parse(payload.data(), payload.size());
  CHECK(m.has_value());
  if (m) {
    CHECK(m->codec == Codec::Opus);
    CHECK(m->sampleRate == 48000);
    CHECK(m->bits == 16);
    CHECK(m->channels == 2);
  }
}

void testCodecHeaderPcm() {
  Bytes wav;
  wav.text("RIFF").u32(36).text("WAVE").text("fmt ").u32(16);
  wav.u16(1).u16(2).u32(44100).u32(44100 * 4).u16(4).u16(16);
  wav.text("data").u32(0);
  const Bytes payload = codecHeader("pcm", wav);
  const auto m = CodecHeaderMessage::parse(payload.data(), payload.size());
  CHECK(m.has_value());
  if (m) {
    CHECK(m->codec == Codec::Pcm);
    CHECK(m->channels == 2);
    CHECK(m->sampleRate == 44100);
    CHECK(m->blockAlign == 4);
    CHECK(m->bits == 16);
  }
}

void testCodecHeaderFlacKeepsItsPayload() {
  Bytes flac;
  flac.text("fLaC").u32(0x22000080);
  const Bytes payload = codecHeader("flac", flac);
  const auto m = CodecHeaderMessage::parse(payload.data(), payload.size());
  CHECK(m.has_value());
  if (m) {
    CHECK(m->codec == Codec::Flac);
    CHECK(std::equal(m->flacPayload.begin(), m->flacPayload.end(),
                     flac.data(), flac.data() + flac.size()));
  }
}

void testCodecHeaderUnknownCodec() {
  const Bytes payload = codecHeader("ogg", Bytes{});
  const auto m = CodecHeaderMessage::parse(payload.data(), payload.size());
  CHECK(m.has_value());
  if (m) {
    CHECK(m->codec == Codec::None);
  }
}

void testCodecHeaderRejectsBadLengths() {
  Bytes opus;
  opus.text("OPUS").u32(48000).u16(16).u16(2);

  for (uint32_t nameLen : {50u, 0xFFFFFFF9u, 0xFFFFFFFFu}) {
    Bytes payload;
    payload.u32(nameLen).text("opus").u32(12).append(opus);
    CHECK(!CodecHeaderMessage::parse(payload.data(), payload.size()));
  }

  Bytes noRoomForCodecLen;
  noRoomForCodecLen.sized("opus").u16(0);
  CHECK(!CodecHeaderMessage::parse(noRoomForCodecLen.data(),
                                   noRoomForCodecLen.size()));

  for (uint32_t codecLen : {13u, 0xFFFFFFF0u, 0xFFFFFFFFu}) {
    Bytes payload;
    payload.sized("opus").u32(codecLen).append(opus);
    CHECK(!CodecHeaderMessage::parse(payload.data(), payload.size()));
  }

  Bytes tooShort;
  tooShort.u16(4).text("o");
  CHECK(!CodecHeaderMessage::parse(tooShort.data(), tooShort.size()));
}

void testWireChunkHeader() {
  Bytes bytes;
  bytes.u32(12).u32(345'678).u32(4608);
  const auto h = WireChunkHeader::parse(bytes.data(), bytes.size());
  CHECK(h.has_value());
  if (h) {
    CHECK(h->timestamp.toMicros() == 12'345'678);
    CHECK(h->payloadSize == 4608);
  }
  CHECK(!WireChunkHeader::parse(bytes.data(), bytes.size() - 1));
}

void testTimeMessageRoundTrip() {
  TimeMessage m;
  m.latency = {-3, -250'000};
  const auto bytes = m.serialize();
  CHECK(bytes.size() == 8);
  const auto parsed = TimeMessage::parse(bytes.data(), bytes.size());
  CHECK(parsed.has_value());
  if (parsed) {
    CHECK(parsed->latency.toMicros() == -3'250'000);
  }
  CHECK(!TimeMessage::parse(bytes.data(), bytes.size() - 1));
}

void testHelloCarriesSnapserversKeys() {
  HelloMessage hello;
  hello.mac = "48:f6:ee:7a:08:7a";
  hello.hostname = "boss";
  hello.version = "1.2.3";
  hello.clientName = "Snapclient";
  hello.os = "ESP-IDF";
  hello.arch = "xtensa";
  hello.instance = 1;
  hello.id = "48:f6:ee:7a:08:7a";
  hello.protocolVersion = 2;

  const auto bytes = hello.serialize();
  CHECK(bytes.size() >= 4);
  const uint32_t jsonLen = std::to_integer<uint32_t>(bytes[0]) |
                           std::to_integer<uint32_t>(bytes[1]) << 8 |
                           std::to_integer<uint32_t>(bytes[2]) << 16 |
                           std::to_integer<uint32_t>(bytes[3]) << 24;
  CHECK(jsonLen == bytes.size() - 4);

  const std::string json(reinterpret_cast<const char*>(bytes.data() + 4),
                         bytes.size() - 4);
  const tao::json::value obj = tao::json::from_string(json);
  CHECK(obj.at("MAC").get_string() == hello.mac);
  CHECK(obj.at("HostName").get_string() == hello.hostname);
  CHECK(obj.at("Version").get_string() == hello.version);
  CHECK(obj.at("ClientName").get_string() == hello.clientName);
  CHECK(obj.at("OS").get_string() == hello.os);
  CHECK(obj.at("Arch").get_string() == hello.arch);
  CHECK(obj.at("Instance").as<int>() == 1);
  CHECK(obj.at("ID").get_string() == hello.id);
  CHECK(obj.at("SnapStreamProtocolVersion").as<int>() == 2);
}

}  // namespace
}  // namespace snapclient

int main() {
  snapclient::testBaseMessageWireLayout();
  snapclient::testServerSettingsFromSnapserver();
  snapclient::testServerSettingsRejectsBadJson();
  snapclient::testServerSettingsClampsVolume();
  snapclient::testServerSettingsRejectsBadLengths();
  snapclient::testCodecHeaderOpus();
  snapclient::testCodecHeaderPcm();
  snapclient::testCodecHeaderFlacKeepsItsPayload();
  snapclient::testCodecHeaderUnknownCodec();
  snapclient::testCodecHeaderRejectsBadLengths();
  snapclient::testWireChunkHeader();
  snapclient::testTimeMessageRoundTrip();
  snapclient::testHelloCarriesSnapserversKeys();

  if (gFailures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", gFailures);
    return 1;
  }
  std::printf("all tests passed\n");
  return 0;
}
