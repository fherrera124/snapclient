#include "snapclient/SnapcastClient.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <new>
#include <optional>
#include <thread>

#include <bell/Logger.h>
#include <tcb/span.hpp>

#include "snapclient/DecoderTask.h"

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#include <esp_system.h>
#endif

namespace snapclient {

namespace {
// Defense against a corrupt/malicious base->size (raw wire uint32_t, up
// to 4GB) driving an unbounded vector resize - real messages (encoded or
// raw PCM chunks, JSON server settings) never approach this.
constexpr uint32_t kMaxMessagePayloadBytes = 65536;

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
    : bell::Task("snapcast_client", 8 * 1024, /*espPriority=*/4,
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
  if (base->size > kMaxMessagePayloadBytes) {
    BELL_LOG(warn, LOG_TAG, "message payload too large: {} bytes",
             base->size);
    return false;
  }
  const int64_t receivedAt = nowUs();
  base->received.sec = static_cast<int32_t>(receivedAt / 1000000);
  base->received.usec = static_cast<int32_t>(receivedAt % 1000000);

  // Read straight into its final destination instead of through the
  // generic buffer below - this is the one message type frequent and big
  // enough (uncompressed Pcm especially) for an extra full-size copy to
  // matter.
  if (base->type == MessageType::WireChunk) {
    if (!readAndHandleWireChunk(base->size)) {
      return false;
    }
    sendTimeSync();
    return true;
  }

  bool ok = true;
  try {
    std::vector<std::byte> payload(base->size);
    if (base->size > 0 && !readExact(payload.data(), payload.size())) {
      BELL_LOG(warn, LOG_TAG, "reading message payload failed");
      return false;
    }

    switch (base->type) {
      case MessageType::CodecHeader:
        ok = handleCodecHeader(payload.data(), payload.size());
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
  } catch (const std::bad_alloc&) {
#ifdef ESP_PLATFORM
    BELL_LOG(warn, LOG_TAG,
             "allocation failed for a {}-byte message: freeHeap={} "
             "largestFreeBlock={}",
             base->size, esp_get_free_heap_size(),
             heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
#else
    BELL_LOG(warn, LOG_TAG, "allocation failed for a {}-byte message",
             base->size);
#endif
    return false;
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

  if (header->codec != Codec::Pcm && header->codec != Codec::Opus &&
      header->codec != Codec::Flac) {
    BELL_LOG(error, LOG_TAG,
             "unsupported codec (only pcm/opus/flac are implemented)");
    return false;
  }

  if (lastActiveCodec_ != Codec::None && header->codec != lastActiveCodec_) {
    // A hot switch reallocates both decoders and DspProcessor's buffers,
    // and bell's EspMemoryResource aborts on OOM rather than failing, so
    // rebooting is the safer path.
    BELL_LOG(warn, LOG_TAG,
             "codec changed ({} -> {}) - restarting for a clean state",
             static_cast<int>(lastActiveCodec_),
             static_cast<int>(header->codec));
#ifdef ESP_PLATFORM
    esp_restart();
#endif
    return false;
  }

  activeCodec_ = header->codec;

  if (activeCodec_ == Codec::Flac) {
    // Flac's real format lives in STREAMINFO, inside flacPayload - not in
    // header->sampleRate/channels/bits, which parse() never populates for
    // it (see CodecHeaderMessage::flacPayload).
    std::lock_guard<std::mutex> lock(flacMutex_);
    auto setupRes =
        flacCodec_.setupDecode(bell::audio::Format{}, bell::audio::FlacConfig{});
    if (!setupRes) {
      BELL_LOG(error, LOG_TAG, "flac setupDecode failed: {}",
               setupRes.error());
      return false;
    }
    auto headerRes = flacCodec_.setupDecodeFromHeaders(tcb::span<const std::byte>(
        header->flacPayload.data(), header->flacPayload.size()));
    if (!headerRes ||
        *headerRes != bell::audio::Codec::SetupStatus::Ready) {
      BELL_LOG(error, LOG_TAG, "flac setupDecodeFromHeaders failed");
      return false;
    }
    pcmFormat_ = flacCodec_.getAudioFormat();
    expectedSamplesPerChunk_ = flacCodec_.getMaxBlockSize();

  } else {
    auto sampleRate = toSampleRate(header->sampleRate);
    if (!sampleRate) {
      BELL_LOG(error, LOG_TAG, "unsupported sample rate {}",
               header->sampleRate);
      return false;
    }
    pcmFormat_ = bell::audio::Format(header->channels,
                                     bell::audio::SampleFormat::S16,
                                     *sampleRate);
    // Pcm has no fixed per-chunk sample count of its own - this is just a
    // bootstrap value until the first real chunk's size corrects it (see
    // DecoderTask's samplesPerChunkHint_ updates).
    expectedSamplesPerChunk_ = kOpusSamplesPerChunk;

    if (activeCodec_ == Codec::Opus) {
      // bufferSize default (100ms) is 5x what a single 20ms/960-sample
      // decode call ever writes (samplesPerPacket defaults to 960 too) -
      // this wastes ~15KB of the tmpBuffer that's never touched.
      bell::audio::OpusConfig opusConfig;
      opusConfig.bufferSize = 4096;
      std::lock_guard<std::mutex> lock(opusMutex_);
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
  }

  receivedCodecHeader_ = true;
  lastActiveCodec_ = activeCodec_;
  if (onCodecReady) {
    onCodecReady(activeCodec_, pcmFormat_);
  }
  return true;
}

bool SnapcastClient::readAndHandleWireChunk(uint32_t totalSize) {
  std::array<std::byte, WireChunkHeader::kWireSize> headerBuf{};
  if (!readExact(headerBuf.data(), headerBuf.size())) {
    BELL_LOG(warn, LOG_TAG, "reading wire chunk header failed");
    return false;
  }
  auto header = WireChunkHeader::parse(headerBuf.data(), headerBuf.size());
  if (!header || totalSize < WireChunkHeader::kWireSize ||
      header->payloadSize != totalSize - WireChunkHeader::kWireSize) {
    BELL_LOG(warn, LOG_TAG, "malformed wire chunk header");
    return false;
  }

  // Only allocate the real destination once the codec (and so the decoder/
  // queue capacity sized for it) is actually known.
  ChunkBuffer buf;
  if (receivedCodecHeader_) {
    buf = acquireChunkBuffer(header->payloadSize);
  }

  if (buf) {
    if (!readExact(buf.data(), buf.size())) {
      BELL_LOG(warn, LOG_TAG, "reading wire chunk payload failed");
      return false;
    }
  } else if (header->payloadSize > 0) {
    // Nothing is playable before the codec header, so that is not a loss.
    if (receivedCodecHeader_) {
      ++payloadAllocFailures_;
      if (payloadLogLimiter_.due(nowUs())) {
        BELL_LOG(warn, LOG_TAG,
                 "audio lost: no memory for a {} byte chunk ({} chunks so "
                 "far, heapFree={}) - lower the server's buffer setting",
                 header->payloadSize, payloadAllocFailures_,
                 chunkHeapFreeBytes());
      }
    }
    // Still must drain these bytes off the socket, or the next message's
    // header would be read from the middle of this one.
    std::array<std::byte, 256> discard;
    size_t remaining = header->payloadSize;
    while (remaining > 0) {
      size_t n = std::min(remaining, discard.size());
      if (!readExact(discard.data(), n)) {
        BELL_LOG(warn, LOG_TAG, "draining wire chunk payload failed");
        return false;
      }
      remaining -= n;
    }
  }

  if (receivedCodecHeader_ && onAudioChunk) {
    onAudioChunk(activeCodec_, std::move(buf), header->timestamp.toMicros());
  }
  return true;
}

bell::Result<SnapcastClient::DecodedView> SnapcastClient::decodeOpus(
    tcb::span<const std::byte> encoded) {
  std::unique_lock<std::mutex> lock(opusMutex_);
  auto decoded = opusCodec_.decode(encoded);
  if (!decoded) {
    return nonstd::make_unexpected(decoded.error());
  }
  return DecodedView(std::move(lock), decoded->pcm);
}

bell::Result<SnapcastClient::DecodedView> SnapcastClient::decodeFlac(
    tcb::span<const std::byte> encoded) {
  std::unique_lock<std::mutex> lock(flacMutex_);
  auto decoded = flacCodec_.decode(encoded);
  if (!decoded) {
    return nonstd::make_unexpected(decoded.error());
  }
  return DecodedView(std::move(lock), decoded->pcm);
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
