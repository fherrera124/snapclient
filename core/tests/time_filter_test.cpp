#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>

#include "snapclient/TimeFilter.h"

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

constexpr uint32_t kSeeds = 20;
constexpr double kDriftPpm = 50.0;

// TIME round trips over WiFi against a server whose clock runs kDriftPpm
// fast: each way takes 1 ms plus exponential jitter, and 2% of them stall
// for another 20-80 ms.
class Link {
 public:
  explicit Link(uint32_t seed) : rng_(seed) {}

  double nowUs() const { return nowUs_; }
  void wait(double us) { nowUs_ += us; }

  // Server time minus client time.
  double offsetAt(double clientUs) const {
    return kBaseOffsetUs + kDriftPpm * clientUs / 1e6;
  }

  double errorAt(const TimeFilter& filter, double clientUs) const {
    return std::fabs(
        static_cast<double>(filter.offsetAt(std::llround(clientUs))) -
        offsetAt(clientUs));
  }

  void roundTrip(TimeFilter& filter) {
    roundTrip(filter, oneWayUs(), oneWayUs());
  }

  // Feeds the filter what SnapcastClient::handleTime() would measure.
  void roundTrip(TimeFilter& filter, double upUs, double downUs) {
    const double measuredUs = offsetAt(nowUs_ + upUs) + (upUs - downUs) / 2;
    nowUs_ += upUs + downUs;
    filter.insert(std::llround(measuredUs), std::llround((upUs + downUs) / 2),
                  std::llround(nowUs_));
  }

 private:
  static constexpr double kBaseOffsetUs = 123'456'789'012.0;

  double uniform() { return (rng_() + 0.5) / 4294967296.0; }

  double oneWayUs() {
    double us = 1000.0 - 500.0 * std::log(uniform());
    if (uniform() < 0.02) {
      us += 20000.0 + uniform() * 60000.0;
    }
    return us;
  }

  std::mt19937 rng_;
  double nowUs_ = 5'000'000.0;
};

// SnapcastClient's pacing: every 10 ms until 29 samples, then every second.
void warmUp(Link& link, TimeFilter& filter) {
  while (!filter.isFull(29)) {
    link.wait(10'000);
    link.roundTrip(filter);
  }
}

void pingFor(Link& link, TimeFilter& filter, int seconds) {
  for (int i = 0; i < seconds; ++i) {
    link.wait(1'000'000);
    link.roundTrip(filter);
  }
}

void testFirstSampleIsTheEstimate() {
  TimeFilter filter;
  filter.insert(5000, 1000, 1'000'000);
  CHECK(filter.offsetAt(1'000'000) == 5000);
  CHECK(filter.offsetAt(60'000'000) == 5000);
}

void testIgnoresSamplesNotNewerThanTheLast() {
  TimeFilter filter;
  filter.insert(5000, 1000, 2'000'000);
  filter.insert(9000, 1000, 2'000'000);
  filter.insert(9000, 1000, 1'000'000);
  CHECK(filter.isFull(1));
  CHECK(!filter.isFull(2));
  CHECK(filter.offsetAt(2'000'000) == 5000);
}

void testResetForgetsEverything() {
  TimeFilter filter;
  filter.insert(5000, 1000, 2'000'000);
  filter.insert(5100, 1000, 3'000'000);
  filter.reset();
  CHECK(!filter.isFull(1));

  filter.insert(-7000, 1000, 1'000'000);
  CHECK(filter.isFull(1));
  CHECK(filter.offsetAt(1'000'000) == -7000);
}

void testExactSamplesKeepEveryMicrosecond() {
  constexpr int64_t kEpochOffsetUs = 1'762'200'000'000'000;
  TimeFilter filter;
  for (int64_t s = 1; s <= 200; ++s) {
    filter.insert(kEpochOffsetUs, /*maxErrorUs=*/0, s * 1'000'000);
    CHECK(filter.offsetAt(s * 1'000'000) == kEpochOffsetUs);
  }
  CHECK(filter.offsetAt(600'000'000) == kEpochOffsetUs);
}

void testSettlesUnderWifiNoise() {
  double worstAtReadyUs = 0;
  double worstSettledUs = 0;
  for (uint32_t seed = 1; seed <= kSeeds; ++seed) {
    Link link(seed);
    TimeFilter filter;
    warmUp(link, filter);
    worstAtReadyUs = std::fmax(worstAtReadyUs,
                               link.errorAt(filter, link.nowUs()));
    worstAtReadyUs = std::fmax(
        worstAtReadyUs, link.errorAt(filter, link.nowUs() + 1'000'000));

    pingFor(link, filter, 300);
    for (int i = 0; i < 300; ++i) {
      pingFor(link, filter, 1);
      worstSettledUs = std::fmax(worstSettledUs,
                                 link.errorAt(filter, link.nowUs()));
    }
  }
  CHECK(worstAtReadyUs < 1000);
  CHECK(worstSettledUs < 200);
}

void testCarriesDriftAcrossAGap() {
  double worstUs = 0;
  for (uint32_t seed = 1; seed <= kSeeds; ++seed) {
    Link link(seed);
    TimeFilter filter;
    warmUp(link, filter);
    pingFor(link, filter, 600);
    // 50 ppm over 30 s is 1500 us.
    worstUs = std::fmax(worstUs,
                        link.errorAt(filter, link.nowUs() + 30'000'000));
  }
  CHECK(worstUs < 200);
}

void testAStalledReplyBarelyMovesTheEstimate() {
  double worstUs = 0;
  for (uint32_t seed = 1; seed <= kSeeds; ++seed) {
    Link link(seed);
    TimeFilter filter;
    warmUp(link, filter);
    pingFor(link, filter, 300);

    link.wait(1'000'000);
    const double replyAtUs = link.nowUs() + 1000 + 51'000;
    const int64_t before = filter.offsetAt(std::llround(replyAtUs));
    link.roundTrip(filter, /*upUs=*/1000, /*downUs=*/51'000);
    worstUs = std::fmax(
        worstUs, std::fabs(static_cast<double>(
                     filter.offsetAt(std::llround(replyAtUs)) - before)));
  }
  CHECK(worstUs < 50);
}

}  // namespace
}  // namespace snapclient

int main() {
  snapclient::testFirstSampleIsTheEstimate();
  snapclient::testIgnoresSamplesNotNewerThanTheLast();
  snapclient::testResetForgetsEverything();
  snapclient::testExactSamplesKeepEveryMicrosecond();
  snapclient::testSettlesUnderWifiNoise();
  snapclient::testCarriesDriftAcrossAGap();
  snapclient::testAStalledReplyBarelyMovesTheEstimate();

  if (gFailures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", gFailures);
    return 1;
  }
  std::printf("all tests passed\n");
  return 0;
}
