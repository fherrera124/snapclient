#include "PcmChunkPool.h"

#include <cstring>

#include "bell/Logger.h"
#include "esp_heap_caps.h"

namespace snapclient {

namespace {
const char* kLogTag = "PcmChunkPool";
}  // namespace

PcmChunkPool::PooledBuffer::PooledBuffer(PooledBuffer&& other) noexcept
    : pool_(other.pool_),
      slotIndex_(other.slotIndex_),
      data_(other.data_),
      size_(other.size_) {
  other.pool_ = nullptr;
  other.data_ = nullptr;
  other.size_ = 0;
}

PcmChunkPool::PooledBuffer& PcmChunkPool::PooledBuffer::operator=(
    PooledBuffer&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  if (pool_) {
    pool_->release(slotIndex_);
  }
  pool_ = other.pool_;
  slotIndex_ = other.slotIndex_;
  data_ = other.data_;
  size_ = other.size_;
  other.pool_ = nullptr;
  other.data_ = nullptr;
  other.size_ = 0;
  return *this;
}

PcmChunkPool::PooledBuffer::~PooledBuffer() {
  if (pool_) {
    pool_->release(slotIndex_);
  }
}

PcmChunkPool::PcmChunkPool(size_t slotCount, size_t slotBytes)
    : slotFree_(slotCount, true),
      slotCount_(slotCount),
      slotBytes_(slotBytes) {
  const size_t totalBytes = slotCount * slotBytes;
  storage_ =
      static_cast<std::byte*>(heap_caps_malloc(totalBytes, MALLOC_CAP_IRAM_8BIT));
  if (storage_ != nullptr) {
    return;
  }
  BELL_LOG(warn, kLogTag,
           "no room in IRAM for {} bytes (largest free IRAM block: {}), "
           "falling back to DRAM",
           totalBytes,
           heap_caps_get_largest_free_block(MALLOC_CAP_IRAM_8BIT));
  storage_ =
      static_cast<std::byte*>(heap_caps_malloc(totalBytes, MALLOC_CAP_8BIT));
  if (storage_ == nullptr) {
    BELL_LOG(error, kLogTag, "failed to allocate {} bytes in DRAM either",
             totalBytes);
  }
}

PcmChunkPool::~PcmChunkPool() {
  if (storage_) {
    heap_caps_free(storage_);
  }
}

PcmChunkPool::PooledBuffer PcmChunkPool::acquire(const std::byte* src,
                                                 size_t len) {
  if (storage_ == nullptr || len > slotBytes_) {
    return {};
  }
  std::lock_guard<std::mutex> lock(mutex_);
  for (size_t i = 0; i < slotCount_; i++) {
    if (slotFree_[i]) {
      slotFree_[i] = false;
      std::byte* dst = storage_ + i * slotBytes_;
      std::memcpy(dst, src, len);
      return PooledBuffer(this, i, dst, len);
    }
  }
  return {};
}

void PcmChunkPool::release(size_t slotIndex) {
  std::lock_guard<std::mutex> lock(mutex_);
  slotFree_[slotIndex] = true;
}

}  // namespace snapclient
