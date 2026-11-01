#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <utility>
#include <vector>

#include <bell/dsp/BiquadParameters.h>

#include "snapclient/I2cBus.h"
#include "snapclient/pcm5122/AlloBossDac.h"
#include "snapclient/pcm5122/Pcm5122Driver.h"
#include "snapclient/pcm5122/Pcm5122Eq.h"

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

// A register file: page 0 writes land in regs and in the ordered log, other
// pages in paged; reads come back from regs.
class FakePcm5122Bus : public I2cBus {
 public:
  std::map<uint8_t, uint8_t> regs;
  std::vector<std::pair<uint8_t, uint8_t>> log;
  std::map<std::pair<uint8_t, uint8_t>, uint8_t> paged;
  // Fails writes by current page and first register.
  std::function<bool(uint8_t page, uint8_t reg)> refuse;

  bool write(uint8_t /*deviceAddr*/, const uint8_t* data,
             size_t len) override {
    if (len < 2 || (len > 2 && (data[0] & 0x80) == 0)) {
      return false;
    }
    if (refuse && refuse(regs[0], data[0] & 0x7F)) {
      return false;
    }
    const uint8_t reg = data[0] & 0x7F;
    for (size_t i = 1; i < len; i++) {
      const auto r = static_cast<uint8_t>(reg + i - 1);
      if (r == 0 || regs[0] == 0) {
        regs[r] = data[i];
        log.emplace_back(r, data[i]);
      } else {
        paged[{regs[0], r}] = data[i];
      }
    }
    return true;
  }

  // Coefficient c as written, sign-extended.
  int32_t coefficient(size_t c) {
    const auto page = static_cast<uint8_t>(44 + c / 30);
    const auto reg = static_cast<uint8_t>(8 + 4 * (c % 30));
    const int32_t word = paged[{page, reg}] << 16 |
                         paged[{page, static_cast<uint8_t>(reg + 1)}] << 8 |
                         paged[{page, static_cast<uint8_t>(reg + 2)}];
    return word & 0x800000 ? word - (1 << 24) : word;
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
constexpr uint8_t kRegDspProgram = 43;
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
  CHECK(bus.regs[kRegMasterClkDiv1] == 15);
  CHECK(bus.regs[kRegMasterClkDiv2] == 63);
  CHECK(bus.regs[kRegIdac1] == 0x04);
  CHECK(bus.regs[kRegIdac2] == 0x00);
  CHECK(bus.regs[kRegI2s1] == 0x03);
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
  CHECK(!boss.start(4000));
  CHECK(bus.regs[kRegPower] == 0x10);
  CHECK(bus.regs.count(kRegGpioControl1) == 0);
}

void test_volume_codes() {
  CHECK(pcm5122VolumeCode(1.0f) == 0x30);
  CHECK(pcm5122VolumeCode(2.0f) == 0x30);  // no boost
  CHECK(pcm5122VolumeCode(0.5f) == 0x3C);  // -6.02 dB, rounds to -6
  CHECK(pcm5122VolumeCode(0.1f) == 0x58);  // -20 dB
  CHECK(pcm5122VolumeCode(0.01f) == 0x80);  // -40 dB
  CHECK(pcm5122VolumeCode(0.0f) == 0xFF);
  CHECK(pcm5122VolumeCode(-1.0f) == 0xFF);
  CHECK(pcm5122VolumeCode(1e-7f) == 0xFF);  // -140 dB, past the range
}

void test_volume_writes_both_channels() {
  FakePcm5122Bus bus;
  Pcm5122Driver dac(bus);
  CHECK(dac.setVolumeCode(0x58));
  CHECK(bus.regs[61] == 0x58);
  CHECK(bus.regs[62] == 0x58);
}

using bell::dsp::BiquadParameters;

int32_t signExtend(uint32_t word) {
  return word & 0x800000 ? static_cast<int32_t>(word) - (1 << 24)
                         : static_cast<int32_t>(word);
}

// What the chip plays at hz: the six biquads, then the volume.
double eqResponseDb(const Pcm5122Eq& eq, double hz, double rate) {
  const auto z1 = std::polar(1.0, -2.0 * M_PI * hz / rate);
  const auto z2 = z1 * z1;
  std::complex<double> h = signExtend(eq.volume) / double(1 << 22);
  for (size_t i = 0; i < Pcm5122Eq::kStages; i++) {
    double w[5];
    for (size_t j = 0; j < 5; j++) {
      w[j] = signExtend(eq.biquads[i * 5 + j]) / double(1 << 23);
    }
    h *= (w[0] + 2.0 * w[1] * z1 + w[2] * z2) /
         (1.0 - 2.0 * w[3] * z1 - w[4] * z2);
  }
  return 20.0 * std::log10(std::abs(h));
}

double eqPeakDb(const Pcm5122Eq& eq, double rate) {
  double peak = -1000.0;
  for (double hz = 10.0; hz < rate / 2; hz *= 1.01) {
    peak = std::max(peak, eqResponseDb(eq, hz, rate));
  }
  return peak;
}

BiquadParameters lowShelf(float hz, float db) {
  return BiquadParameters(BiquadParameters::Type::Lowshelf, hz, 0.707f, db,
                          std::nullopt, std::nullopt);
}

BiquadParameters highShelf(float hz, float db) {
  return BiquadParameters(BiquadParameters::Type::Highshelf, hz, 0.707f, db,
                          std::nullopt, std::nullopt);
}

void test_eq_flat_is_identity() {
  const auto eq = pcm5122Eq({}, 44100);
  CHECK(eq.has_value());
  for (size_t i = 0; i < Pcm5122Eq::kStages; i++) {
    CHECK(eq->biquads[i * 5] == 0x7FFFFF);
    for (size_t j = 1; j < 5; j++) {
      CHECK(eq->biquads[i * 5 + j] == 0);
    }
  }
  CHECK(eq->volume == 0x400000);
}

void test_eq_cut_matches_bench_words() {
  // Measured on the Boss at -4.5 dB, within the meter's error.
  const int32_t bench[5] = {0x7A2E38, 0x8CD86B - (1 << 24), 0x6E7C78,
                            0x732795, 0x975550 - (1 << 24)};
  const auto eq = pcm5122Eq(
      {BiquadParameters(BiquadParameters::Type::Peaking, 1000.0f, 1.0f,
                        -6.0f, std::nullopt, std::nullopt)},
      44100);
  CHECK(eq.has_value());
  for (size_t j = 0; j < 5; j++) {
    CHECK(std::abs(signExtend(eq->biquads[j]) - bench[j]) <= 8);
  }
  CHECK(eq->volume == 0x400000);
}

void test_eq_boost_keeps_headroom() {
  const auto eq = pcm5122Eq({lowShelf(100.0f, 9.0f)}, 44100);
  CHECK(eq.has_value());
  CHECK(std::abs(eqPeakDb(*eq, 44100)) < 0.05);
  CHECK(std::abs(eqResponseDb(*eq, 5000, 44100) + 9.0) < 0.1);
}

void test_eq_makes_up_for_coefficient_scaling() {
  // Cancel out, but the boost's coefficients only fit scaled down.
  const auto eq =
      pcm5122Eq({highShelf(8000.0f, -6.0f), highShelf(8000.0f, 6.0f)}, 48000);
  CHECK(eq.has_value());
  CHECK(signExtend(eq->volume) > 0x400000);
  CHECK(std::abs(eqResponseDb(*eq, 100, 48000)) < 0.1);
  CHECK(std::abs(eqResponseDb(*eq, 15000, 48000)) < 0.1);
}

void test_eq_rejects_make_up_past_6db() {
  CHECK(!pcm5122Eq({highShelf(4000.0f, -12.0f), highShelf(4000.0f, 12.0f)},
                   48000)
             .has_value());
}

void test_eq_rejects_seven_stages() {
  CHECK(!pcm5122Eq(std::vector<BiquadParameters>(7, lowShelf(100.0f, 3.0f)),
                   44100)
             .has_value());
}

void test_boss_eq_waits_for_start() {
  FakePcm5122Bus bus;
  Pcm5122Driver driver(bus);
  AlloBossDac boss(driver);
  CHECK(!boss.setEq({lowShelf(100.0f, 6.0f)}));
  CHECK(boss.setEq({}));
  CHECK(bus.paged.empty());
}

void test_boss_start_loads_eq() {
  FakePcm5122Bus bus;
  Pcm5122Driver driver(bus);
  AlloBossDac boss(driver);
  const std::vector<BiquadParameters> stages = {lowShelf(100.0f, 6.0f)};
  boss.setEq(stages);
  CHECK(boss.start(44100));
  const auto eq = pcm5122Eq(stages, 44100);
  CHECK(eq.has_value());
  for (size_t j = 0; j < 5; j++) {
    CHECK(bus.coefficient(10 + j) == signExtend(eq->biquads[j]));
  }
  CHECK(bus.coefficient(15) == 0x7FFFFF);
  CHECK(bus.coefficient(148) == 0x7FFFFF);
  CHECK(bus.coefficient(158) == signExtend(eq->volume));
  CHECK(bus.regs[0] == 0);
  CHECK(bus.regs[kRegDspProgram] == 5);
  CHECK(bus.log.back() == std::make_pair(kRegPower, uint8_t{0}));
  CHECK(boss.setEq(stages));
}

void test_boss_eq_off_above_48k() {
  FakePcm5122Bus bus;
  Pcm5122Driver driver(bus);
  AlloBossDac boss(driver);
  boss.setEq({lowShelf(100.0f, 6.0f)});
  CHECK(boss.start(96000));
  CHECK(bus.regs[kRegDspProgram] == 1);
  CHECK(!boss.setEq({lowShelf(100.0f, 6.0f)}));
  CHECK(bus.paged.empty());
}

void test_boss_eq_change_runs_in_standby() {
  FakePcm5122Bus bus;
  Pcm5122Driver driver(bus);
  AlloBossDac boss(driver);
  CHECK(boss.start(48000));
  CHECK(bus.regs[kRegDspProgram] == 1);
  bus.log.clear();
  CHECK(boss.setEq({lowShelf(100.0f, 6.0f)}));
  CHECK(bus.log.front() == std::make_pair(kRegPower, uint8_t{0x10}));
  CHECK(bus.log.back() == std::make_pair(kRegPower, uint8_t{0}));
  CHECK(bus.regs[kRegDspProgram] == 5);
  CHECK(boss.setEq({}));
  CHECK(bus.regs[kRegDspProgram] == 1);
}

void test_boss_eq_change_without_standby_keeps_old_eq() {
  FakePcm5122Bus bus;
  Pcm5122Driver driver(bus);
  AlloBossDac boss(driver);
  boss.setEq({lowShelf(100.0f, 6.0f)});
  CHECK(boss.start(48000));
  bus.refuse = [](uint8_t page, uint8_t reg) {
    return page == 0 && reg == kRegPower;
  };
  CHECK(!boss.setEq({}));
  CHECK(!boss.setEq({lowShelf(100.0f, 3.0f)}));
  CHECK(bus.regs[kRegDspProgram] == 5);
  bus.refuse = nullptr;
  CHECK(boss.setEq({}));
  CHECK(bus.regs[kRegDspProgram] == 1);
}

void test_boss_retries_after_unknown_eq_state() {
  FakePcm5122Bus bus;
  Pcm5122Driver driver(bus);
  AlloBossDac boss(driver);
  boss.setEq({lowShelf(100.0f, 6.0f)});
  CHECK(boss.start(48000));
  bus.refuse = [](uint8_t page, uint8_t reg) {
    return (page >= 44 && reg >= 8) || (page == 0 && reg == kRegDspProgram);
  };
  CHECK(!boss.setEq({lowShelf(100.0f, 3.0f)}));
  CHECK(bus.regs[kRegDspProgram] == 5);
  bus.refuse = nullptr;
  CHECK(boss.setEq({}));
  CHECK(bus.regs[kRegDspProgram] == 1);
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
  snapclient::test_volume_codes();
  snapclient::test_volume_writes_both_channels();
  snapclient::test_eq_flat_is_identity();
  snapclient::test_eq_cut_matches_bench_words();
  snapclient::test_eq_boost_keeps_headroom();
  snapclient::test_eq_makes_up_for_coefficient_scaling();
  snapclient::test_eq_rejects_make_up_past_6db();
  snapclient::test_eq_rejects_seven_stages();
  snapclient::test_boss_eq_waits_for_start();
  snapclient::test_boss_start_loads_eq();
  snapclient::test_boss_eq_off_above_48k();
  snapclient::test_boss_eq_change_runs_in_standby();
  snapclient::test_boss_eq_change_without_standby_keeps_old_eq();
  snapclient::test_boss_retries_after_unknown_eq_state();

  if (gFailures > 0) {
    std::fprintf(stderr, "%d failure(s)\n", gFailures);
    return 1;
  }
  std::printf("pcm5122_driver_test: all passed\n");
  return 0;
}
