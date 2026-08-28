#include "snapclient/ChunkBuffer.h"

#include <cstdlib>
#include <cstring>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

namespace snapclient {

namespace {
// Real Opus (<=1275B, RFC 6716) and Pcm (960 frames * 4B = 3840B) chunks
// never approach this - defense against a corrupt or malicious
// caller-supplied len, not a functional ceiling.
constexpr size_t kMaxChunkBytes = 8192;

#ifdef ESP_PLATFORM
std::byte* allocateChunkMemory(size_t len) {
  // PSRAM tier: a no-op fast-fail on any board without CONFIG_SPIRAM=y -
  // only relevant where PSRAM is actually registered as heap.
  if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) >= len) {
    if (auto* p = static_cast<std::byte*>(
            heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))) {
      return p;
    }
  }
  if (heap_caps_get_free_size(MALLOC_CAP_8BIT) >= len) {
    if (auto* p = static_cast<std::byte*>(
            heap_caps_malloc(len, MALLOC_CAP_8BIT))) {
      return p;
    }
  }
  return nullptr;
}

void freeChunkMemory(std::byte* p) {
  heap_caps_free(p);
}
#else
std::byte* allocateChunkMemory(size_t len) {
  return static_cast<std::byte*>(std::malloc(len));
}

void freeChunkMemory(std::byte* p) {
  std::free(p);
}
#endif
}  // namespace

ChunkBuffer::ChunkBuffer(ChunkBuffer&& other) noexcept
    : data_(other.data_), size_(other.size_), valid_(other.valid_) {
  other.data_ = nullptr;
  other.size_ = 0;
  other.valid_ = false;
}

ChunkBuffer& ChunkBuffer::operator=(ChunkBuffer&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  if (valid_ && data_) {
    freeChunkMemory(data_);
  }
  data_ = other.data_;
  size_ = other.size_;
  valid_ = other.valid_;
  other.data_ = nullptr;
  other.size_ = 0;
  other.valid_ = false;
  return *this;
}

ChunkBuffer::~ChunkBuffer() {
  if (valid_ && data_) {
    freeChunkMemory(data_);
  }
}

ChunkBuffer acquireChunkBuffer(const std::byte* src, size_t len) {
  if (len == 0) {
    return ChunkBuffer(nullptr, 0);
  }
  if (len > kMaxChunkBytes) {
    return {};
  }
  std::byte* dst = allocateChunkMemory(len);
  if (!dst) {
    return {};
  }
  std::memcpy(dst, src, len);
  return ChunkBuffer(dst, len);
}

size_t chunkHeapFreeBytes() {
#ifdef ESP_PLATFORM
  return heap_caps_get_free_size(MALLOC_CAP_8BIT);
#else
  return 0;
#endif
}

size_t chunkHeapLargestFreeBlockBytes() {
#ifdef ESP_PLATFORM
  return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
#else
  return 0;
#endif
}

}  // namespace snapclient
