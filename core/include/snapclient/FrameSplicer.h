#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>

namespace snapclient {

// Lengthens or shortens 16-bit stereo PCM by a few frames and leaves every
// other frame bit-exact. Each edit goes where the signal changes least
// between two neighbouring frames, one edit per equal slice of the chunk:
// removing a frame merges that pair into its midpoint, adding one inserts
// the midpoint between them.
//
// 'in' is read only as whole aligned 32-bit words - one stereo frame is
// exactly four bytes - so it may point into memory that rejects narrower
// access, such as a pool slot in ESP32 IRAM.
class FrameSplicer {
 public:
  static void process(const int16_t* in, size_t inFrames, int16_t* out,
                      size_t outFrames) {
    const auto* frames = reinterpret_cast<const uint32_t*>(in);
    size_t written = 0;
    auto emit = [&](uint32_t frame) {
      out[written * 2] = static_cast<int16_t>(left(frame));
      out[written * 2 + 1] = static_cast<int16_t>(right(frame));
      written++;
    };

    const bool grow = outFrames > inFrames;
    const size_t edits = grow ? outFrames - inFrames : inFrames - outFrames;
    const size_t pairs = inFrames > 0 ? inFrames - 1 : 0;
    // Two pairs per slice, so a removal never reaches into the next slice.
    if (edits == 0 || pairs < 2 * edits) {
      for (size_t i = 0; i < outFrames; i++) {
        emit(inFrames > 0 ? frames[i < inFrames ? i : inFrames - 1] : 0);
      }
      return;
    }

    size_t next = 0;
    for (size_t e = 0; e < edits; e++) {
      const size_t end = pairs * (e + 1) / edits;
      size_t best = std::max(pairs * e / edits, next);
      uint32_t bestCost = std::numeric_limits<uint32_t>::max();
      for (size_t p = best; p < end; p++) {
        const uint32_t cost = change(frames[p], frames[p + 1]);
        if (cost < bestCost) {
          bestCost = cost;
          best = p;
        }
      }
      for (; next < best; next++) {
        emit(frames[next]);
      }
      const uint32_t mid = midpoint(frames[best], frames[best + 1]);
      if (grow) {
        emit(frames[best]);
        emit(mid);
        next = best + 1;
      } else {
        emit(mid);
        next = best + 2;
      }
    }
    for (; next < inFrames; next++) {
      emit(frames[next]);
    }
  }

 private:
  static int32_t left(uint32_t frame) {
    return static_cast<int16_t>(frame & 0xFFFF);
  }
  static int32_t right(uint32_t frame) {
    return static_cast<int16_t>(frame >> 16);
  }
  static uint32_t change(uint32_t a, uint32_t b) {
    return static_cast<uint32_t>(std::abs(left(b) - left(a)) +
                                 std::abs(right(b) - right(a)));
  }
  static uint32_t midpoint(uint32_t a, uint32_t b) {
    const auto l = static_cast<uint16_t>((left(a) + left(b)) >> 1);
    const auto r = static_cast<uint16_t>((right(a) + right(b)) >> 1);
    return uint32_t{l} | uint32_t{r} << 16;
  }
};

}  // namespace snapclient
