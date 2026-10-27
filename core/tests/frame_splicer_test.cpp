#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "snapclient/FrameSplicer.h"

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

using Pcm = std::vector<int16_t>;

Pcm splice(const Pcm& in, int adjustment) {
  const size_t inFrames = in.size() / 2;
  const size_t outFrames = inFrames + adjustment;
  Pcm out(outFrames * 2);
  FrameSplicer::process(in.data(), inFrames, out.data(), outFrames);
  return out;
}

// Left rises 60 per frame except for a gentle step of 1 after each frame
// in flat, so the cheapest pair in each slice is known. Right mirrors it.
Pcm steepWithFlatSpots(size_t frames, std::initializer_list<size_t> flat) {
  Pcm pcm(frames * 2);
  int32_t value = -30000;
  for (size_t i = 0; i < frames; i++) {
    pcm[i * 2] = static_cast<int16_t>(value);
    pcm[i * 2 + 1] = static_cast<int16_t>(-value);
    bool gentle = false;
    for (size_t f : flat) {
      gentle |= f == i;
    }
    value += gentle ? 1 : 60;
  }
  return pcm;
}

int16_t mid(int16_t a, int16_t b) {
  return static_cast<int16_t>((int32_t{a} + b) >> 1);
}

void test_no_adjustment_is_bit_exact() {
  Pcm in(960 * 2);
  for (size_t i = 0; i < in.size(); i++) {
    in[i] = static_cast<int16_t>(32000.0 * std::sin(i * 0.37));
  }
  CHECK(splice(in, 0) == in);
}

void test_removal_merges_the_flattest_pair() {
  const Pcm in = steepWithFlatSpots(960, {700});
  const Pcm out = splice(in, -1);
  CHECK(out.size() == 959 * 2);
  for (size_t i = 0; i < 700; i++) {
    CHECK(out[i * 2] == in[i * 2] && out[i * 2 + 1] == in[i * 2 + 1]);
  }
  CHECK(out[700 * 2] == mid(in[700 * 2], in[701 * 2]));
  CHECK(out[700 * 2 + 1] == mid(in[700 * 2 + 1], in[701 * 2 + 1]));
  for (size_t i = 701; i < 959; i++) {
    CHECK(out[i * 2] == in[(i + 1) * 2]);
    CHECK(out[i * 2 + 1] == in[(i + 1) * 2 + 1]);
  }
}

void test_addition_inserts_a_midpoint_at_the_flattest_pair() {
  const Pcm in = steepWithFlatSpots(960, {123});
  const Pcm out = splice(in, 1);
  CHECK(out.size() == 961 * 2);
  for (size_t i = 0; i <= 123; i++) {
    CHECK(out[i * 2] == in[i * 2] && out[i * 2 + 1] == in[i * 2 + 1]);
  }
  CHECK(out[124 * 2] == mid(in[123 * 2], in[124 * 2]));
  CHECK(out[124 * 2 + 1] == mid(in[123 * 2 + 1], in[124 * 2 + 1]));
  for (size_t i = 124; i < 960; i++) {
    CHECK(out[(i + 1) * 2] == in[i * 2]);
    CHECK(out[(i + 1) * 2 + 1] == in[i * 2 + 1]);
  }
}

void test_several_edits_take_one_spot_per_slice() {
  // Three slices of the 959 pairs: [0,319), [319,638), [638,959).
  const Pcm in = steepWithFlatSpots(960, {50, 400, 900});
  const Pcm out = splice(in, -3);
  CHECK(out.size() == 957 * 2);
  CHECK(out[50 * 2] == mid(in[50 * 2], in[51 * 2]));
  CHECK(out[(400 - 1) * 2] == mid(in[400 * 2], in[401 * 2]));
  CHECK(out[(900 - 2) * 2] == mid(in[900 * 2], in[901 * 2]));
  CHECK(out[956 * 2] == in[959 * 2]);

  const Pcm grown = splice(in, 3);
  CHECK(grown.size() == 963 * 2);
  CHECK(grown[51 * 2] == mid(in[50 * 2], in[51 * 2]));
  CHECK(grown[(401 + 1) * 2] == mid(in[400 * 2], in[401 * 2]));
  CHECK(grown[(901 + 2) * 2] == mid(in[900 * 2], in[901 * 2]));
  CHECK(grown[962 * 2] == in[959 * 2]);
}

void test_back_to_back_removals_stay_in_their_slices() {
  // The flattest pair of the first slice is its last one.
  const Pcm in = steepWithFlatSpots(9, {3, 4});
  const Pcm out = splice(in, -2);
  CHECK(out.size() == 7 * 2);
  CHECK(out[3 * 2] == mid(in[3 * 2], in[4 * 2]));
  CHECK(out[4 * 2] == mid(in[5 * 2], in[6 * 2]));
  CHECK(out[6 * 2] == in[8 * 2]);
}

void test_midpoint_does_not_overflow() {
  Pcm in(8 * 2, INT16_MAX);
  in[4 * 2] = INT16_MIN;
  in[4 * 2 + 1] = INT16_MIN;
  const Pcm out = splice(in, -1);
  for (int16_t s : out) {
    CHECK(s == INT16_MAX || s == -1 || s == 0 || s == INT16_MIN);
  }
}

void test_short_chunks_fall_back_to_the_tail() {
  const Pcm in = {1, -1, 2, -2, 3, -3};
  const Pcm shorter = splice(in, -2);
  CHECK((shorter == Pcm{1, -1}));
  const Pcm longer = splice(in, 2);
  CHECK((longer == Pcm{1, -1, 2, -2, 3, -3, 3, -3, 3, -3}));
  Pcm none;
  Pcm out(2 * 2, 7);
  FrameSplicer::process(none.data(), 0, out.data(), 2);
  CHECK((out == Pcm{0, 0, 0, 0}));
}

}  // namespace
}  // namespace snapclient

int main() {
  snapclient::test_no_adjustment_is_bit_exact();
  snapclient::test_removal_merges_the_flattest_pair();
  snapclient::test_addition_inserts_a_midpoint_at_the_flattest_pair();
  snapclient::test_several_edits_take_one_spot_per_slice();
  snapclient::test_back_to_back_removals_stay_in_their_slices();
  snapclient::test_midpoint_does_not_overflow();
  snapclient::test_short_chunks_fall_back_to_the_tail();

  if (gFailures == 0) {
    std::printf("all tests passed\n");
    return 0;
  }
  std::fprintf(stderr, "%d check(s) failed\n", gFailures);
  return 1;
}
