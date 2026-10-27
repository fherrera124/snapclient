#include <cstdint>
#include <cstdio>
#include <map>
#include <utility>
#include <vector>

#include "snapclient/I2cBus.h"
#include "snapclient/pcm5122/AlloBossDac.h"
#include "snapclient/pcm5122/Pcm5122Driver.h"

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

// A register file: writes land in regs and in the ordered log, reads come
// back from regs.
class FakePcm5122Bus : public I2cBus {
 public:
  std::map<uint8_t, uint8_t> regs;
  std::vector<std::pair<uint8_t, uint8_t>> log;

  bool write(uint8_t /*deviceAddr*/, const uint8_t* data,
             size_t len) override {
    if (len != 2) {
      return false;
    }
    regs[data[0]] = data[1];
    log.emplace_back(data[0], data[1]);
    return true;
  }

  bool writeThenRead(uint8_t /*deviceAddr*/, const uint8_t* writeData,
                     size_t writeLen, uint8_t* readBuf,
                     size_t readLen) override {
    if (writeLen != 1 || readLen != 1) {
      return false;
    }
    readBuf[0] = regs.count(writeData[0]) ? regs[writeData[0]] : 0;
    return true;
  }
};

constexpr uint8_t kRegPower = 2;
constexpr uint8_t kRegGpioEn = 8;
constexpr uint8_t kRegMasterClkDiv1 = 32;
constexpr uint8_t kRegMasterClkDiv2 = 33;
constexpr uint8_t kRegIdac1 = 35;
constexpr uint8_t kRegIdac2 = 36;
constexpr uint8_t kRegI2s1 = 40;
constexpr uint8_t kRegGpioControl1 = 86;
constexpr uint8_t kRegRateDet4 = 94;

void test_dividers_48k_from_49m152() {
  auto d = pcm5122MasterDividers(49'152'000, 48000, 32);
  CHECK(d.has_value());
  CHECK(d->bck == 32);
  CHECK(d->lrck == 32);
  CHECK(d->dsp == 1);
  CHECK(d->dac == 8);
  CHECK(d->osr == 8);
  CHECK(d->ncp == 4);
  CHECK(d->idac == 1024);
  CHECK(d->fsSpeed == 0);
}

void test_dividers_44k1_from_45m1584_match_48k() {
  auto d = pcm5122MasterDividers(45'158'400, 44100, 32);
  CHECK(d.has_value());
  CHECK(d->bck == 32);
  CHECK(d->dac == 8);
  CHECK(d->osr == 8);
  CHECK(d->ncp == 4);
  CHECK(d->idac == 1024);
}

void test_dividers_96k() {
  auto d = pcm5122MasterDividers(49'152'000, 96000, 32);
  CHECK(d.has_value());
  CHECK(d->bck == 16);
  CHECK(d->dac == 8);
  CHECK(d->osr == 4);
  CHECK(d->idac == 512);
  CHECK(d->fsSpeed == 1);
}

void test_dividers_reject_inexact_or_too_slow() {
  CHECK(!pcm5122MasterDividers(45'158'400, 48000, 32).has_value());
  // BCK divider would be 192, past the register's 128.
  CHECK(!pcm5122MasterDividers(49'152'000, 8000, 32).has_value());
  CHECK(!pcm5122MasterDividers(49'152'000, 0, 32).has_value());
}

void test_boss_start_48k_picks_gpio3_oscillator() {
  FakePcm5122Bus bus;
  Pcm5122Driver driver(bus);
  AlloBossDac boss(driver);
  CHECK(boss.start(48000));
  CHECK(bus.regs[kRegGpioEn] == 0x2C);
  CHECK(bus.regs[kRegGpioControl1] == (0x04 | 0x08));
  CHECK(bus.regs[kRegMasterClkDiv1] == 31);
  CHECK(bus.regs[kRegMasterClkDiv2] == 31);
  CHECK(bus.regs[kRegIdac1] == 0x04);
  CHECK(bus.regs[kRegIdac2] == 0x00);
  CHECK(bus.regs[kRegI2s1] == 0x00);
  // Out of standby only as the very last step.
  CHECK(!bus.log.empty());
  CHECK(bus.log.back() == std::make_pair(kRegPower, uint8_t{0}));
}

void test_boss_start_44k1_picks_gpio6_oscillator() {
  FakePcm5122Bus bus;
  Pcm5122Driver driver(bus);
  AlloBossDac boss(driver);
  CHECK(boss.start(44100));
  CHECK(bus.regs[kRegGpioControl1] == (0x20 | 0x08));
}

void test_boss_start_without_sck_stays_in_standby() {
  FakePcm5122Bus bus;
  bus.regs[kRegRateDet4] = 0x40;
  Pcm5122Driver driver(bus);
  AlloBossDac boss(driver);
  CHECK(!boss.start(48000));
  CHECK(bus.regs[kRegPower] == 0x10);
  CHECK(bus.regs.count(kRegMasterClkDiv1) == 0);
}

void test_boss_start_unsupported_rate_stays_in_standby() {
  FakePcm5122Bus bus;
  bus.regs[kRegPower] = 0;
  Pcm5122Driver driver(bus);
  AlloBossDac boss(driver);
  CHECK(!boss.start(8000));
  CHECK(bus.regs[kRegPower] == 0x10);
  CHECK(bus.regs.count(kRegGpioControl1) == 0);
}

}  // namespace
}  // namespace snapclient

int main() {
  snapclient::test_dividers_48k_from_49m152();
  snapclient::test_dividers_44k1_from_45m1584_match_48k();
  snapclient::test_dividers_96k();
  snapclient::test_dividers_reject_inexact_or_too_slow();
  snapclient::test_boss_start_48k_picks_gpio3_oscillator();
  snapclient::test_boss_start_44k1_picks_gpio6_oscillator();
  snapclient::test_boss_start_without_sck_stays_in_standby();
  snapclient::test_boss_start_unsupported_rate_stays_in_standby();

  if (gFailures > 0) {
    std::fprintf(stderr, "%d failure(s)\n", gFailures);
    return 1;
  }
  std::printf("pcm5122_driver_test: all passed\n");
  return 0;
}
