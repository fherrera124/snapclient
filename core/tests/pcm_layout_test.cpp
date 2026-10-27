#include <cstdio>

#include "snapclient/PcmLayout.h"

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

void test_layouts_from_block_align() {
  auto s16 = pcmLayoutFor(16, 2, 4);
  CHECK(s16 && s16->isS16());
  // Snapcast's 24-bit: S24_LE, 4 bytes per sample.
  auto s24 = pcmLayoutFor(24, 2, 8);
  CHECK(s24 && s24->bits == 24 && s24->containerBytes == 4);
  auto s24packed = pcmLayoutFor(24, 2, 6);
  CHECK(s24packed && s24packed->containerBytes == 3);
  auto s32 = pcmLayoutFor(32, 2, 8);
  CHECK(s32 && s32->bits == 32 && !s32->isS16());
}

void test_rejects_what_cannot_narrow() {
  CHECK(!pcmLayoutFor(8, 2, 2));
  CHECK(!pcmLayoutFor(24, 2, 4));  // container smaller than the bits
  CHECK(!pcmLayoutFor(24, 2, 7));  // not a whole sample per channel
  CHECK(!pcmLayoutFor(16, 0, 4));
}

}  // namespace
}  // namespace snapclient

int main() {
  snapclient::test_layouts_from_block_align();
  snapclient::test_rejects_what_cannot_narrow();

  if (gFailures == 0) {
    std::printf("all tests passed\n");
    return 0;
  }
  std::fprintf(stderr, "%d check(s) failed\n", gFailures);
  return 1;
}
