#include "snapclient/SnapcastClient.h"

#include <array>
#include <chrono>
#include <optional>
#include <thread>

#include <bell/Logger.h>
#include <tcb/span.hpp>

#ifdef ESP_PLATFORM
#include <esp_system.h>
#endif

namespace snapclient {

namespace {
int64_t nowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::optional<bell::audio::SampleRate> toSampleRate(uint32_t hz) {
  switch (hz) {
    case 8000:
      return bell::audio::SampleRate::SR_8000HZ;
    case 16000:
      return bell::audio::SampleRate::SR_16000HZ;
    case 22050:
      return bell::audio::SampleRate::SR_22050HZ;
    case 44100:
      return bell::audio::SampleRate::SR_44100HZ;
    case 48000:
      return bell::audio::SampleRate::SR_48000HZ;
    default:
      return std::nullopt;
  }
}
}  // namespace

SnapcastClient::SnapcastClient(Config config)
    : bell::Task("snapcast_client", 32768, /*espPriority=*/4,
                bell::TaskCore::CoreAny, /*espStackOnPsram=*/false),
      config_(std::move(config)) {
  startTask();
}

SnapcastClient::~SnapcastClient() {
  stopTask();
}

void SnapcastClient::setPingIntervalUs(int64_t intervalUs) {
  pingIntervalUs_ = intervalUs;
}

void SnapcastClient::taskLoop() {
  if (!connected_) {
    if (connectAndHandshake()) {
      connected_ = true;
      return;
    }
  } else if (readAndDispatchOne()) {
    return;
  }
  disconnect();
  std::this_thread::sleep_for(std::chrono::seconds(1));
}

void SnapcastClient::wakeTask() {
  socket_.close();
}

void SnapcastClient::disconnect() {
  socket_.close();
  connected_ = false;
  receivedCodecHeader_ = false;
  activeCodec_ = Codec::None;
}

bool SnapcastClient::writeAll(const std::vector<std::byte>& data) {
  size_t sent = 0;
  while (sent < data.size()) {
    auto res = socket_.write(data.data() + sent, data.size() - sent);
    if (!res || *res == 0) {
      return false;
    }
    sent += *res;
  }
  return true;
}

bool SnapcastClient::readExact(std::byte* buf, size_t len) {
  size_t got = 0;
  while (got < len) {
    auto res = socket_.read(buf + got, len - got);
    if (!res || *res == 0) {
      return false;
    }
    got += *res;
  }
  return true;
}

bool SnapcastClient::connectAndHandshake() {
  auto res = socket_.connect(config_.host, config_.port, 5000);
  if (!res) {
    BELL_LOG(warn, LOG_TAG, "connect to {}:{} failed: {}", config_.host,
             config_.port, res.error());
    return false;
  }

  HelloMessage hello;
  hello.mac = "02:00:00:00:00:01";
  hello.hostname = config_.clientName;
  hello.version = "0.1.0";
  hello.clientName = "snapclient-cpp";
  hello.os = "linux";
  hello.arch = "x86_64";
  hello.instance = 1;
  hello.id = hello.mac;
  hello.protocolVersion = 2;

  auto payload = hello.serialize();
  BaseMessage base;
  base.type = MessageType::Hello;
  base.id = nextId_++;
  base.size = static_cast<uint32_t>(payload.size());

  if (!writeAll(base.serialize()) || !writeAll(payload)) {
    BELL_LOG(warn, LOG_TAG, "sending hello to {}:{} failed", config_.host,
             config_.port);
    return false;
  }

  receivedCodecHeader_ = false;
  activeCodec_ = Codec::None;
  lastTimeSyncSentUs_ = 0;
  pingIntervalUs_ = 10000;
  if (onConnected) {
    onConnected();
  }
  return true;
}

bool SnapcastClient::readAndDispatchOne() {
  std::array<std::byte, BaseMessage::kWireSize> headerBuf{};
  if (!readExact(headerBuf.data(), headerBuf.size())) {
    BELL_LOG(warn, LOG_TAG, "reading message header failed");
    return false;
  }
  auto base = BaseMessage::parse(headerBuf.data(), headerBuf.size());
  if (!base) {
    BELL_LOG(warn, LOG_TAG, "malformed message header");
    return false;
  }
  const int64_t receivedAt = nowUs();
  base->received.sec = static_cast<int32_t>(receivedAt / 1000000);
  base->received.usec = static_cast<int32_t>(receivedAt % 1000000);

  std::vector<std::byte> payload(base->size);
  if (base->size > 0 && !readExact(payload.data(), payload.size())) {
    BELL_LOG(warn, LOG_TAG, "reading message payload failed");
    return false;
  }

  bool ok = true;
  switch (base->type) {
    case MessageType::CodecHeader:
      ok = handleCodecHeader(payload.data(), payload.size());
      break;
    case MessageType::WireChunk:
      handleWireChunk(payload.data(), payload.size());
      break;
    case MessageType::ServerSettings:
      handleServerSettings(payload.data(), payload.size());
      break;
    case MessageType::Time:
      handleTime(*base, payload.data(), payload.size());
      break;
    default:
      break;
  }

  sendTimeSync();
  return ok;
}

void SnapcastClient::sendTimeSync() {
  if (!receivedCodecHeader_) {
    return;
  }
  const int64_t now = nowUs();
  if (now - lastTimeSyncSentUs_ < pingIntervalUs_) {
    return;
  }
  lastTimeSyncSentUs_ = now;

  TimeMessage msg;
  msg.latency = {0, 0};
  auto payload = msg.serialize();

  BaseMessage base;
  base.type = MessageType::Time;
  base.id = nextId_++;
  base.sent.sec = static_cast<int32_t>(now / 1000000);
  base.sent.usec = static_cast<int32_t>(now % 1000000);
  base.size = static_cast<uint32_t>(payload.size());

  writeAll(base.serialize());
  writeAll(payload);
}

bool SnapcastClient::handleCodecHeader(const std::byte* payload, size_t len) {
  auto header = CodecHeaderMessage::parse(payload, len);
  if (!header) {
    BELL_LOG(error, LOG_TAG, "malformed codec header");
    return false;
  }

  if (header->codec != Codec::Pcm && header->codec != Codec::Opus) {
    BELL_LOG(error, LOG_TAG,
             "unsupported codec (only pcm/opus are implemented)");
    return false;
  }

  auto sampleRate = toSampleRate(header->sampleRate);
  if (!sampleRate) {
    BELL_LOG(error, LOG_TAG, "unsupported sample rate {}",
             header->sampleRate);
    return false;
  }

  activeCodec_ = header->codec;
  pcmFormat_ = bell::audio::Format(header->channels,
                                   bell::audio::SampleFormat::S16,
                                   *sampleRate);

  if (activeCodec_ == Codec::Opus) {
    // bufferSize default (100ms) is 5x what a single 20ms/960-sample
    // decode call ever writes (samplesPerPacket defaults to 960 too) -
    // this wastes ~15KB of the tmpBuffer that's never touched.
    bell::audio::OpusConfig opusConfig;
    opusConfig.bufferSize = 4096;
    auto setupRes = opusCodec_.setupDecode(pcmFormat_, opusConfig);
    if (!setupRes) {
      BELL_LOG(error, LOG_TAG, "opus setupDecode failed: {}",
               setupRes.error());
      return false;
    }
#ifdef ESP_PLATFORM
    BELL_LOG(info, LOG_TAG, "opus setupDecode done: freeHeap={}",
             esp_get_free_heap_size());
#endif
  }

  receivedCodecHeader_ = true;
  if (onCodecReady) {
    onCodecReady(pcmFormat_);
  }
  return true;
}

void SnapcastClient::handleWireChunk(const std::byte* payload, size_t len) {
  if (!receivedCodecHeader_) {
    return;
  }
  auto chunk = WireChunkMessage::parse(payload, len);
  if (!chunk) {
    return;
  }

  if (activeCodec_ == Codec::Pcm) {
    if (onPcmChunk) {
      onPcmChunk(chunk->payload.data(), chunk->payload.size(),
                chunk->timestamp.toMicros());
    }
    return;
  }

  auto decoded = opusCodec_.decode(
      tcb::span<const std::byte>(chunk->payload.data(), chunk->payload.size()));
  if (!decoded) {
    BELL_LOG(warn, LOG_TAG, "opus decode failed: {}", decoded.error());
    return;
  }
  if (onPcmChunk) {
    onPcmChunk(decoded->pcm.data(), decoded->pcm.size(),
              chunk->timestamp.toMicros());
  }
}

void SnapcastClient::handleServerSettings(const std::byte* payload,
                                          size_t len) {
  auto msg = ServerSettingsMessage::parse(payload, len);
  if (!msg) {
    return;
  }
  if (onServerSettings) {
    onServerSettings({msg->bufferMs, msg->latency, msg->volume, msg->muted});
  }
}

void SnapcastClient::handleTime(const BaseMessage& base,
                                const std::byte* payload, size_t len) {
  auto msg = TimeMessage::parse(payload, len);
  if (!msg) {
    return;
  }

  const int64_t trx = base.received.toMicros();
  const int64_t ttx = base.sent.toMicros();
  const int64_t tdif = trx - ttx;
  const int64_t serverLatency = msg->latency.toMicros();
  const int64_t offsetUs = (serverLatency - tdif) / 2;
  const int64_t maxErrorUs = (tdif + serverLatency) / 2;

  if (onTimeSample) {
    onTimeSample(offsetUs, maxErrorUs, trx);
  }
}

}  // namespace snapclient
