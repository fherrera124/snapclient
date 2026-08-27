#pragma once

#include <cstddef>
#include <mutex>
#include <vector>

namespace snapclient {

// Fixed-capacity pool of equally-sized byte buffers, allocated once at
// construction. Exists to eliminate the per-chunk malloc/free churn that
// fragments the heap under sustained streaming - callers get a slot back
// as an RAII handle (PooledBuffer); returning it to the pool happens
// automatically when the handle is destroyed or overwritten by a move.
// Slot contents are whatever the caller stores - encoded audio payloads,
// decoded PCM, or anything else of a known max size.
class PcmChunkPool {
 public:
  class PooledBuffer {
   public:
    PooledBuffer() = default;
    PooledBuffer(const PooledBuffer&) = delete;
    PooledBuffer& operator=(const PooledBuffer&) = delete;
    PooledBuffer(PooledBuffer&& other) noexcept;
    PooledBuffer& operator=(PooledBuffer&& other) noexcept;
    ~PooledBuffer();

    const std::byte* data() const { return data_; }
    size_t size() const { return size_; }
    explicit operator bool() const { return data_ != nullptr; }

   private:
    friend class PcmChunkPool;
    PooledBuffer(PcmChunkPool* pool, size_t slotIndex, std::byte* data,
                size_t size)
        : pool_(pool), slotIndex_(slotIndex), data_(data), size_(size) {}

    PcmChunkPool* pool_ = nullptr;
    size_t slotIndex_ = 0;
    std::byte* data_ = nullptr;
    size_t size_ = 0;
  };

  PcmChunkPool(size_t slotCount, size_t slotBytes);
  ~PcmChunkPool();
  PcmChunkPool(const PcmChunkPool&) = delete;
  PcmChunkPool& operator=(const PcmChunkPool&) = delete;

  // Copies src[0..len) into a free slot and returns it. Never blocks;
  // returns an empty (falsy) PooledBuffer if len exceeds the pool's slot
  // size or no slot is currently free.
  PooledBuffer acquire(const std::byte* src, size_t len);

 private:
  void release(size_t slotIndex);

  // Backing storage for all slots - allocated from IRAM (a pool separate
  // from the DRAM heap Opus/DSP/WiFi/HTTP draw from) when there's room
  // there, falling back to DRAM otherwise. Freed with heap_caps_free(),
  // not delete[], so this is a raw pointer rather than unique_ptr.
  std::byte* storage_ = nullptr;
  std::vector<bool> slotFree_;
  std::mutex mutex_;
  size_t slotCount_;
  size_t slotBytes_;
};

}  // namespace snapclient
