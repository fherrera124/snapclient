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
  friend ChunkBuffer acquireChunkBuffer(const std::byte* src, size_t len);
  ChunkBuffer(std::byte* data, size_t size)
      : data_(data), size_(size), valid_(true) {}

  std::byte* data_ = nullptr;
  size_t size_ = 0;
  bool valid_ = false;
};

// Copies src[0..len) into a new heap buffer sized exactly to len. Never
// blocks or throws - tries PSRAM first where available, then the default
// heap, and gives up immediately rather than stall the network thread
// this runs on. Returns a falsy ChunkBuffer on failure.
ChunkBuffer acquireChunkBuffer(const std::byte* src, size_t len);

// Diagnostics for periodic logging only, never the hot per-chunk path -
// chunkHeapLargestFreeBlockBytes() walks the whole heap under a global
// lock. Both report 0 on host builds (no esp_heap_caps.h there).
size_t chunkHeapFreeBytes();
size_t chunkHeapLargestFreeBlockBytes();

}  // namespace snapclient
