#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <bell/audio/Common.h>
#include <bell/audio/FlacCodec.h>
#include <bell/audio/OpusCodec.h>
#include <bell/net/TCPSocket.h>
#include <bell/utils/Task.h>
#include <tcb/span.hpp>

#include "snapclient/ChunkBuffer.h"
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
// Delivers PCM and Opus payloads as received, undecoded - decoding is the
// caller's own responsibility (via decodeOpus() below), on the caller's
// own schedule, so a caller buffering many chunks ahead of playback holds
// compact encoded data instead of one decoded PCM buffer per chunk.
// FLAC/OGG/unknown codecs disconnect (triggers a retry, same as any other
// connection error).
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
  // Fires once per codec header (initial connect or a mid-stream codec
  // change). A caller queuing undecoded chunks ahead of playback should
  // drop anything still queued from before this fires - decodeOpus()
  // decodes against whichever codec instance was set up by the most
  // recent onCodecReady, and a stale queued chunk from before that isn't
  // guaranteed to decode against it correctly.
  std::function<void(Codec codec, const bell::audio::Format&)> onCodecReady;
  // codec is what payload is encoded with: Pcm is usable as-is, Opus
  // needs decodeOpus() first. payload is falsy if allocating for it
  // failed - still fired so the caller can account for the frames.
  std::function<void(Codec codec, ChunkBuffer payload, int64_t serverTimeUs)>
      onAudioChunk;
  std::function<void(const ServerSettings&)> onServerSettings;
  std::function<void(int64_t offsetUs, int64_t maxErrorUs, int64_t nowUs)>
      onTimeSample;

  // Caller (owner of the SyncEngine) switches this from the fast warm-up
  // cadence to a slower steady-state one once latency samples are enough.
  void setPingIntervalUs(int64_t intervalUs);

  // Decodes one Opus payload delivered via onAudioChunk into out (must
  // have room for outCapacity bytes), returning the number of PCM bytes
  // written. Safe to call from any single thread, including one other
  // than whichever called onAudioChunk - internally serialized against a
  // codec change (which tears down and recreates the decoder) via a
  // mutex, since that now happens on this object's own task while this
  // is expected to be called from the caller's playback thread instead.
  bell::Result<size_t> decodeOpus(tcb::span<const std::byte> encoded,
                                  std::byte* out, size_t outCapacity);

  // Same contract as decodeOpus(), for the Flac codec.
  bell::Result<size_t> decodeFlac(tcb::span<const std::byte> encoded,
                                  std::byte* out, size_t outCapacity);

  // Best known samples-per-chunk for the active codec: exact for Opus
  // (960, fixed), the FLAC stream's STREAMINFO max block size for Flac -
  // set once at codec-header time, before any chunk has actually been
  // decoded. Callers wanting the continuously-refined value should track
  // real decoded/observed chunk sizes themselves instead (see
  // PlaybackPipeline's samplesPerChunkHint_).
  uint32_t expectedSamplesPerChunk() const { return expectedSamplesPerChunk_; }

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
  // Guards opusCodec_ - a codec change (this object's own task, inside
  // handleCodecHeader) recreates the decoder, which would otherwise race
  // a caller's decodeOpus() call (a different thread).
  std::mutex opusMutex_;
  bell::audio::FlacCodec flacCodec_;
  // Guards flacCodec_, same reasoning as opusMutex_ above.
  std::mutex flacMutex_;
  bell::audio::Format pcmFormat_;
  Codec activeCodec_ = Codec::None;
  uint32_t expectedSamplesPerChunk_ = 0;

  bool connectAndHandshake();
  bool readAndDispatchOne();
  bool readExact(std::byte* buf, size_t len);
  bool writeAll(const std::vector<std::byte>& data);
  void sendTimeSync();
  void disconnect();

  bool handleCodecHeader(const std::byte* payload, size_t len);
  // Reads and handles one WireChunk message's payload straight off the
  // socket into its final destination buffer - totalSize is the whole
  // message's byte count (WireChunkHeader::kWireSize + audio payload), as
  // already read from the BaseMessage header. false only for a socket read
  // failure (the connection itself is broken); a payload allocation
  // failure is a separate, non-fatal condition still reported via
  // onAudioChunk with a falsy payload.
  bool readAndHandleWireChunk(uint32_t totalSize);
  void handleServerSettings(const std::byte* payload, size_t len);
  void handleTime(const BaseMessage& base, const std::byte* payload,
                  size_t len);
};

}  // namespace snapclient
