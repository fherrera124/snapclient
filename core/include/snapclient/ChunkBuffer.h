#pragma once

#include <cstddef>

namespace snapclient {

// Owns a heap buffer for one audio chunk's payload, sized to exactly what
// the chunk needed. Move-only; frees its buffer (if any) on destruction.
class ChunkBuffer {
 public:
  ChunkBuffer() = default;
  ChunkBuffer(const ChunkBuffer&) = delete;
  ChunkBuffer& operator=(const ChunkBuffer&) = delete;
  ChunkBuffer(ChunkBuffer&& other) noexcept;
  ChunkBuffer& operator=(ChunkBuffer&& other) noexcept;
  ~ChunkBuffer();

  const std::byte* data() const { return data_; }
  std::byte* data() { return data_; }
  size_t size() const { return size_; }
  // False only for a chunk acquireChunkBuffer() couldn't get memory for -
  // a genuinely empty (0-byte) chunk is still true.
  explicit operator bool() const { return valid_; }

 private:
  friend ChunkBuffer acquireChunkBuffer(size_t len);
  friend ChunkBuffer acquireChunkBuffer(const std::byte* src, size_t len);
  ChunkBuffer(std::byte* data, size_t size)
      : data_(data), size_(size), valid_(true) {}

  std::byte* data_ = nullptr;
  size_t size_ = 0;
  bool valid_ = false;
};

// Reserves a new heap buffer sized exactly to len, left uninitialized - for
// a caller about to fill it directly (e.g. reading a socket payload into
// it) instead of copying from an already-in-memory source. Never blocks or
// throws - tries PSRAM first where available, then the default heap, and
// gives up immediately rather than stall the network thread this runs on.
// Returns a falsy ChunkBuffer on failure.
ChunkBuffer acquireChunkBuffer(size_t len);

// Copies src[0..len) into a new heap buffer sized exactly to len. Same
// failure/allocation behavior as the size-only overload above.
ChunkBuffer acquireChunkBuffer(const std::byte* src, size_t len);

// As above, but retries for a few tens of milliseconds, letting chunks
// already playing free the block this needs. Blocks: decoder thread only.
ChunkBuffer acquireChunkBufferRetrying(const std::byte* src, size_t len);

// A cheap counter read (no heap walk) - safe to call from a hot per-chunk
// path. 0 on host builds (no esp_heap_caps.h there).
size_t chunkHeapFreeBytes();

// Diagnostics for periodic logging only, never the hot per-chunk path -
// walks the whole heap under a global lock. 0 on host builds.
size_t chunkHeapLargestFreeBlockBytes();

}  // namespace snapclient
