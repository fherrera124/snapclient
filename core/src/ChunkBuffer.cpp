#include "snapclient/ChunkBuffer.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include <bell/Logger.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

#include "snapclient/RateLimiter.h"

namespace snapclient {

namespace {
// Real Opus (<=1275B, RFC 6716) and Pcm (960 frames * 4B = 3840B) chunks
// never approach this - defense against a corrupt or malicious
// caller-supplied len, not a functional ceiling.
constexpr size_t kMaxChunkBytes = 8192;

const char* kLogTag = "ChunkBuffer";

std::atomic<size_t> gPoolMisses{0};
std::atomic<int64_t> gLastMissLogUs{0};

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

// Leftover instruction RAM reaches the heap as a region that only
// tolerates aligned 32-bit access, and nothing else competes for it - so
// slots take it before the 8-bit heap the encoded payloads need.
std::byte* allocatePoolSlot(size_t len, bool& wordOnly) {
  wordOnly = false;
  if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) >= len) {
    if (auto* p = static_cast<std::byte*>(
            heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))) {
      return p;
    }
  }
  if (auto* p = static_cast<std::byte*>(
          heap_caps_malloc(len, MALLOC_CAP_32BIT | MALLOC_CAP_EXEC))) {
    wordOnly = true;
    return p;
  }
  return allocateChunkMemory(len);
}
#else
std::byte* allocateChunkMemory(size_t len) {
  return static_cast<std::byte*>(std::malloc(len));
}

void freeChunkMemory(std::byte* p) {
  std::free(p);
}

std::byte* allocatePoolSlot(size_t len, bool& wordOnly) {
  wordOnly = false;
  return allocateChunkMemory(len);
}
#endif

// Never touches a byte, so it is safe on a slot in the 32-bit-only
// region. Both pointers must be 4-aligned and len a multiple of 4.
void copyWords(std::byte* dst, const std::byte* src, size_t len) {
  auto* d = reinterpret_cast<uint32_t*>(dst);
  const auto* s = reinterpret_cast<const uint32_t*>(src);
  for (size_t i = 0; i < len / 4; ++i) {
    d[i] = s[i];
  }
}

class ChunkPool {
 public:
  size_t configure(size_t slotBytes, size_t slotCount) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slotBytes_ == slotBytes && owned_.size() >= slotCount) {
      return owned_.size();
    }
    if (free_.size() != owned_.size()) {
      return owned_.size();
    }
    for (auto* p : owned_) {
      freeChunkMemory(p);
    }
    owned_.clear();
    free_.clear();
    wordOnly_ = 0;
    slotBytes_ = slotBytes;
    for (size_t i = 0; i < slotCount; ++i) {
      bool wordOnly = false;
      std::byte* p = allocatePoolSlot(slotBytes, wordOnly);
      if (!p) {
        break;
      }
      owned_.push_back(p);
      free_.push_back(p);
      wordOnly_ += wordOnly ? 1 : 0;
    }
    return owned_.size();
  }

  std::byte* acquire(size_t len) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (len > slotBytes_ || free_.empty()) {
      return nullptr;
    }
    std::byte* p = free_.back();
    free_.pop_back();
    return p;
  }

  void release(std::byte* p) {
    std::lock_guard<std::mutex> lock(mutex_);
    free_.push_back(p);
  }

  size_t slots() {
    std::lock_guard<std::mutex> lock(mutex_);
    return owned_.size();
  }

  size_t wordOnlySlots() {
    std::lock_guard<std::mutex> lock(mutex_);
    return wordOnly_;
  }

 private:
  std::mutex mutex_;
  std::vector<std::byte*> owned_;
  std::vector<std::byte*> free_;
  size_t slotBytes_ = 0;
  size_t wordOnly_ = 0;
};

ChunkPool gPool;

void releaseBuffer(std::byte* data, bool pooled) {
  if (pooled) {
    gPool.release(data);
  } else {
    freeChunkMemory(data);
  }
}
}  // namespace

ChunkBuffer::ChunkBuffer(ChunkBuffer&& other) noexcept
    : data_(other.data_),
      size_(other.size_),
      valid_(other.valid_),
      pooled_(other.pooled_) {
  other.data_ = nullptr;
  other.size_ = 0;
  other.valid_ = false;
  other.pooled_ = false;
}

ChunkBuffer& ChunkBuffer::operator=(ChunkBuffer&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  if (valid_ && data_) {
    releaseBuffer(data_, pooled_);
  }
  data_ = other.data_;
  size_ = other.size_;
  valid_ = other.valid_;
  pooled_ = other.pooled_;
  other.data_ = nullptr;
  other.size_ = 0;
  other.valid_ = false;
  other.pooled_ = false;
  return *this;
}

ChunkBuffer::~ChunkBuffer() {
  if (valid_ && data_) {
    releaseBuffer(data_, pooled_);
  }
}

ChunkBuffer acquireChunkBuffer(size_t len) {
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
  return ChunkBuffer(dst, len);
}

ChunkBuffer acquireChunkBuffer(const std::byte* src, size_t len) {
  ChunkBuffer buf = acquireChunkBuffer(len);
  if (buf && len > 0) {
    std::memcpy(buf.data(), src, len);
  }
  return buf;
}

size_t configureChunkPool(size_t slotBytes, size_t slotCount) {
  return gPool.configure(slotBytes, slotCount);
}

size_t chunkPoolSlots() { return gPool.slots(); }

size_t chunkPoolWordOnlySlots() { return gPool.wordOnlySlots(); }

ChunkBuffer acquirePooledChunkBuffer(const std::byte* src, size_t len) {
  if (len % 4 == 0) {
    if (std::byte* slot = gPool.acquire(len)) {
      copyWords(slot, src, len);
      return ChunkBuffer(slot, len, /*pooled=*/true);
    }
  }

  const size_t total = gPoolMisses.fetch_add(1, std::memory_order_relaxed) + 1;
  const int64_t now = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
  if (now - gLastMissLogUs.load(std::memory_order_relaxed) >=
      kLossLogIntervalUs) {
    gLastMissLogUs.store(now, std::memory_order_relaxed);
    BELL_LOG(warn, kLogTag, "chunk pool missed for {} bytes ({} total, {} slots)",
             len, total, gPool.slots());
  }
  return acquireChunkBuffer(src, len);
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
