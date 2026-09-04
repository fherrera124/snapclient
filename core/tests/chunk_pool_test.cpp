#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "snapclient/ChunkBuffer.h"
#include "snapclient/DynamicResampler.h"

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

// The sample-at-a-time reference DynamicResampler must match now that it
// reads its input as 32-bit words.
void resampleByHalfWords(const int16_t* in, size_t inFrames, int16_t* out,
                         size_t outFrames) {
  if (inFrames == outFrames) {
    for (size_t i = 0; i < inFrames * 2; ++i) {
      out[i] = in[i];
    }
    return;
  }
  uint32_t step = (inFrames << 16) / outFrames;
  uint32_t phase = 0;
  for (size_t i = 0; i < outFrames; ++i) {
    uint32_t index = phase >> 16;
    uint32_t frac = phase & 0xFFFF;
    uint32_t nextIndex = (index + 1 < inFrames) ? index + 1 : index;
    int32_t l0 = in[index * 2];
    int32_t l1 = in[nextIndex * 2];
    out[i * 2] = static_cast<int16_t>(l0 + (((l1 - l0) * static_cast<int32_t>(frac)) >> 16));
    int32_t r0 = in[index * 2 + 1];
    int32_t r1 = in[nextIndex * 2 + 1];
    out[i * 2 + 1] = static_cast<int16_t>(r0 + (((r1 - r0) * static_cast<int32_t>(frac)) >> 16));
    phase += step;
  }
}

void testResamplerMatchesHalfWordReads() {
  for (size_t inFrames : {size_t{960}, kFlacFrames}) {
    std::vector<int16_t> in(inFrames * 2);
    for (size_t i = 0; i < inFrames; ++i) {
      // Both signs, so a botched sign-extension out of the packed word
      // shows up rather than cancelling.
      in[i * 2] = static_cast<int16_t>(32000.0 * std::sin(i * 0.05));
      in[i * 2 + 1] = static_cast<int16_t>(-31000.0 * std::sin(i * 0.031));
    }
    for (int adjustment : {-8, -1, 0, 1, 8}) {
      const size_t outFrames = inFrames + adjustment;
      std::vector<int16_t> actual(outFrames * 2), expected(outFrames * 2);
      DynamicResampler::process(in.data(), inFrames, actual.data(), outFrames);
      resampleByHalfWords(in.data(), inFrames, expected.data(), outFrames);
      CHECK(actual == expected);
    }
  }
}

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
  snapclient::testResamplerMatchesHalfWordReads();
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
