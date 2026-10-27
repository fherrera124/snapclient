#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "snapclient/ChunkBuffer.h"

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

constexpr size_t kFlacFrames = 1152;
constexpr size_t kSlotBytes = kFlacFrames * 4;

std::vector<int16_t> makePcm() {
  std::vector<int16_t> pcm(kFlacFrames * 2);
  for (size_t i = 0; i < pcm.size(); ++i) {
    pcm[i] = static_cast<int16_t>(i * 7);
  }
  return pcm;
}

void testPoolServesAndRecycles() {
  const std::vector<int16_t> pcm = makePcm();
  const auto* src = reinterpret_cast<const std::byte*>(pcm.data());

  CHECK(configureChunkPool(kSlotBytes, 6) == 6);
  CHECK(chunkPoolSlots() == 6);

  {
    std::vector<ChunkBuffer> held;
    for (int i = 0; i < 6; ++i) {
      held.push_back(acquirePooledChunkBuffer(src, kSlotBytes));
      CHECK(static_cast<bool>(held.back()));
      CHECK(std::memcmp(held.back().data(), src, kSlotBytes) == 0);
    }
    // Exhausted, but the caller still gets a usable buffer.
    ChunkBuffer overflow = acquirePooledChunkBuffer(src, kSlotBytes);
    CHECK(static_cast<bool>(overflow));
  }

  // Everything above went out of scope, so the slots are back.
  for (int round = 0; round < 3; ++round) {
    std::vector<ChunkBuffer> held;
    for (int i = 0; i < 6; ++i) {
      held.push_back(acquirePooledChunkBuffer(src, kSlotBytes));
      CHECK(static_cast<bool>(held.back()));
    }
  }
}

void testReconfigureWaitsForSlotsToReturn() {
  const std::vector<int16_t> pcm = makePcm();
  const auto* src = reinterpret_cast<const std::byte*>(pcm.data());

  CHECK(configureChunkPool(kSlotBytes, 6) == 6);
  {
    ChunkBuffer out = acquirePooledChunkBuffer(src, kSlotBytes);
    // Freeing slabs with one still handed out would be a use-after-free.
    CHECK(configureChunkPool(4096, 3) == 6);
  }
  CHECK(configureChunkPool(4096, 3) == 3);
}

void testUnpoolableLengthsBypassThePool() {
  const std::vector<int16_t> pcm = makePcm();
  const auto* src = reinterpret_cast<const std::byte*>(pcm.data());

  CHECK(configureChunkPool(kSlotBytes, 6) == 6);
  ChunkBuffer odd = acquirePooledChunkBuffer(src, kSlotBytes - 2);
  CHECK(static_cast<bool>(odd));
  ChunkBuffer tooBig = acquirePooledChunkBuffer(src, kSlotBytes + 4);
  CHECK(static_cast<bool>(tooBig));
  CHECK(chunkPoolSlots() == 6);
}

}  // namespace
}  // namespace snapclient

int main() {
  snapclient::testPoolServesAndRecycles();
  snapclient::testReconfigureWaitsForSlotsToReturn();
  snapclient::testUnpoolableLengthsBypassThePool();

  if (gFailures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", gFailures);
    return 1;
  }
  std::printf("all tests passed\n");
  return 0;
}
