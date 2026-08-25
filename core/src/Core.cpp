#include "snapclient/Core.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <vector>

#include <bell/Logger.h>

#include "snapclient/ControlSettings.h"
#include "snapclient/DspProcessor.h"
#include "snapclient/JsonFileSettingsStore.h"
#include "snapclient/UdpLogBackend.h"
#include "snapclient/tas5805m/Tas5805mDriver.h"
#include "snapclient/tas5805m/Tas5805mSettings.h"

namespace snapclient {

static const char* LOG_TAG = "snapclient";

void scaffoldSelfCheck() {
  BELL_LOG(info, LOG_TAG, "snapclient_core linked against bell, scaffold ok");
}

namespace {
constexpr float kPi = 3.14159265358979323846f;

int16_t peakOnChannel(const std::vector<int16_t>& interleaved, int channel) {
  int16_t peak = 0;
  for (size_t i = channel; i < interleaved.size(); i += 2) {
    peak = std::max(peak, static_cast<int16_t>(std::abs(interleaved[i])));
  }
  return peak;
}

struct FlowCase {
  DspFlow flow;
  const char* name;
  DspFilterParams params;
};

// Records every I2cBus transaction instead of talking to real hardware, so
// Tas5805mDriver's register/book-page logic can be asserted against without
// a chip. readValues feeds writeThenRead()'s read half in call order.
class RecordingI2cBus : public I2cBus {
 public:
  std::vector<std::vector<uint8_t>> writes;
  std::vector<std::vector<uint8_t>> reads;
  std::vector<uint8_t> readValues;

  bool write(uint8_t /*deviceAddr*/, const uint8_t* data,
            size_t len) override {
    writes.emplace_back(data, data + len);
    return true;
  }

  bool writeThenRead(uint8_t /*deviceAddr*/, const uint8_t* writeData,
                     size_t writeLen, uint8_t* readBuf,
                     size_t readLen) override {
    reads.emplace_back(writeData, writeData + writeLen);
    for (size_t i = 0; i < readLen; i++) {
      readBuf[i] = (readIndex_ < readValues.size()) ? readValues[readIndex_++]
                                                     : 0;
    }
    return true;
  }

 private:
  size_t readIndex_ = 0;
};

uint32_t swapEndian32ForTest(uint32_t val) {
  return ((val & 0xFF) << 24) | ((val & 0xFF00) << 8) |
         ((val & 0xFF0000) >> 8) | ((val >> 24) & 0xFF);
}

// Recomputes the Q9.23 encoding locally so expected bytes can be asserted
// bit-exactly.
uint32_t expectedQ9_23(float value) {
  auto fixedVal = static_cast<int32_t>(value * static_cast<float>(1 << 23));
  return swapEndian32ForTest(static_cast<uint32_t>(fixedVal));
}

std::vector<uint8_t> le32(uint32_t v) {
  std::vector<uint8_t> out(4);
  std::memcpy(out.data(), &v, 4);
  return out;
}
}  // namespace

bool dspSmokeTest() {
  constexpr size_t kFrames = 512;
  constexpr float kToneHz = 100.0f;
  constexpr float kSampleRate = 44100.0f;
  constexpr int16_t kAmplitude = 8000;

  std::vector<int16_t> input(kFrames * 2);
  for (size_t i = 0; i < kFrames; i++) {
    auto sample = static_cast<int16_t>(
        kAmplitude * sinf(2.0f * kPi * kToneHz * static_cast<float>(i) /
                          kSampleRate));
    input[i * 2] = sample;
    input[i * 2 + 1] = sample;
  }
  std::vector<int16_t> output(kFrames * 2);
  int16_t inPeak = peakOnChannel(input, 0);

  const FlowCase cases[] = {
      {DspFlow::Stereo, "Stereo", {}},
      {DspFlow::EQBassTreble, "EQBassTreble", {150.0f, 6.0f, 8000.0f, 3.0f}},
      {DspFlow::BassBoost, "BassBoost", {150.0f, 9.0f, 0.0f, 0.0f}},
      {DspFlow::Biamp, "Biamp", {500.0f, 0.0f, 500.0f, 0.0f}},
  };

  bool allOk = true;
  for (const auto& c : cases) {
    DspProcessor dsp;
    dsp.setParams(c.flow, c.params);
    dsp.switchFlow(c.flow);

    bool ok = true;
    try {
      dsp.process(reinterpret_cast<const std::byte*>(input.data()),
                  input.size() * sizeof(int16_t),
                  reinterpret_cast<std::byte*>(output.data()),
                  output.size() * sizeof(int16_t),
                  bell::audio::SampleRate::SR_44100HZ);
    } catch (const std::exception& e) {
      BELL_LOG(error, LOG_TAG, "{}: threw: {}", c.name, e.what());
      ok = false;
    }

    if (ok) {
      int16_t outPeak0 = peakOnChannel(output, 0);
      int16_t outPeak1 = peakOnChannel(output, 1);
      BELL_LOG(info, LOG_TAG, "{}: in={} out_ch0={} out_ch1={}", c.name,
               inPeak, outPeak0, outPeak1);

      switch (c.flow) {
        case DspFlow::Biamp:
          // ch0 (lowpass, 500Hz) should pass a 100Hz tone through mostly
          // unattenuated; ch1 (highpass, 500Hz) should attenuate it heavily.
          ok = outPeak1 < inPeak / 2;
          break;
        case DspFlow::BassBoost:
        case DspFlow::EQBassTreble:
          // A boosted low shelf should raise a 100Hz tone's peak.
          ok = outPeak0 > inPeak;
          break;
        case DspFlow::Stereo:
          break;
      }
    }

    BELL_LOG(info, LOG_TAG, "{}: {}", c.name, ok ? "PASS" : "FAIL");
    allOk = allOk && ok;
  }

  return allOk;
}

bool settingsSmokeTest() {
  namespace fs = std::filesystem;
  std::string path =
      (fs::temp_directory_path() / "snapclient_settings_smoketest.json")
          .string();
  std::error_code ec;
  fs::remove(path, ec);

  bool ok = true;

  {
    JsonFileSettingsStore store(path);
    ControlSettings settings(store);
    ok = settings.applyJson(
             R"({"server":{"host":"192.168.1.50","port":4444},)"
             R"("dsp":{"activeFlow":"biamp","flows":{"biamp":)"
             R"({"freqPrimaryHz":600,"gainPrimaryDb":1,)"
             R"("freqTertiaryHz":700,"gainTertiaryDb":2}}},)"
             R"("logging":{"enabled":true,"udpHost":"192.168.1.99",)"
             R"("udpPort":9999}})") &&
         ok;
    ok = (settings.serverHost() == "192.168.1.50") && ok;
    ok = (settings.serverPort() == 4444) && ok;
    ok = (settings.activeFlow() == DspFlow::Biamp) && ok;
    ok = (settings.udpLogEnabled() == true) && ok;
    ok = (settings.udpLogHost() == "192.168.1.99") && ok;
    ok = (settings.udpLogPort() == 9999) && ok;
  }

  {
    // Fresh store/settings against the same file: verifies persistence
    // across reconstruction.
    JsonFileSettingsStore store(path);
    ControlSettings settings(store);
    ok = (settings.serverHost() == "192.168.1.50") && ok;
    ok = (settings.serverPort() == 4444) && ok;
    ok = (settings.activeFlow() == DspFlow::Biamp) && ok;
    ok = (settings.udpLogEnabled() == true) && ok;
    ok = (settings.udpLogHost() == "192.168.1.99") && ok;
    auto params = settings.flowParams(DspFlow::Biamp);
    ok = (params.freqPrimaryHz == 600.0f) && ok;
    ok = (params.gainPrimaryDb == 1.0f) && ok;

    ok = !settings.applyJson(R"({"dsp":{"activeFlow":"nope"}})") && ok;
    ok = !settings.applyJson(R"({"server":{"port":"not-a-number"}})") && ok;
    ok = !settings.applyJson(R"({"logging":{"enabled":"not-a-bool"}})") && ok;
    ok = (settings.activeFlow() == DspFlow::Biamp) && ok;
    ok = (settings.serverPort() == 4444) && ok;
    ok = (settings.udpLogEnabled() == true) && ok;

    ok = settings.applyJson(R"({"logging":{"enabled":false}})") && ok;
    ok = (settings.udpLogEnabled() == false) && ok;
    ok = (settings.udpLogHost() == "192.168.1.99") && ok;
  }

  fs::remove(path, ec);

  BELL_LOG(info, LOG_TAG, "settingsSmokeTest: {}", ok ? "PASS" : "FAIL");
  return ok;
}

bool tas5805mDriverSmokeTest() {
  bool allOk = true;

  {
    RecordingI2cBus bus;
    Tas5805mDriver driver(bus, 0x2D);
    bool ok = driver.setState(Tas5805mState::Play);
    ok = ok && bus.writes.size() == 1 &&
         bus.writes[0] == std::vector<uint8_t>{0x03, 0x03};
    BELL_LOG(info, LOG_TAG, "tas5805mDriverSmokeTest setState: {}",
             ok ? "PASS" : "FAIL");
    allOk = allOk && ok;
  }

  {
    RecordingI2cBus bus;
    Tas5805mDriver driver(bus, 0x2D);
    bool ok = driver.setDigitalVolume(0x30);
    ok = ok && bus.writes.size() == 1 &&
         bus.writes[0] == std::vector<uint8_t>{0x4c, 0x30};
    BELL_LOG(info, LOG_TAG, "tas5805mDriverSmokeTest setDigitalVolume: {}",
             ok ? "PASS" : "FAIL");
    allOk = allOk && ok;
  }

  {
    RecordingI2cBus bus;
    Tas5805mDriver driver(bus, 0x2D);
    bool ok = driver.setAnalogGain(5);
    ok = ok && bus.writes.size() == 1 &&
         bus.writes[0] == std::vector<uint8_t>{0x54, 0x05};
    BELL_LOG(info, LOG_TAG, "tas5805mDriverSmokeTest setAnalogGain: {}",
             ok ? "PASS" : "FAIL");
    allOk = allOk && ok;
  }

  {
    // Read-modify-write: DEVICE_CTRL_1 starts at 0x00, PBTL sets bit 2.
    RecordingI2cBus bus;
    bus.readValues = {0x00};
    Tas5805mDriver driver(bus, 0x2D);
    bool ok = driver.setDacMode(Tas5805mDacMode::Pbtl);
    ok = ok && bus.reads.size() == 1 &&
         bus.reads[0] == std::vector<uint8_t>{0x02};
    ok = ok && bus.writes.size() == 1 &&
         bus.writes[0] == std::vector<uint8_t>{0x02, 0x04};
    BELL_LOG(info, LOG_TAG, "tas5805mDriverSmokeTest setDacMode: {}",
             ok ? "PASS" : "FAIL");
    allOk = allOk && ok;
  }

  {
    // DEVICE_CTRL_1 starts saturated (0xFF); Hybrid|384kHz should clear
    // bits 0-1/4-6 then OR in mode=2, freq=0x10 -> 0x9E. BD freq writes
    // ANA_CTRL_REGISTER outright (not read-modify-write).
    RecordingI2cBus bus;
    bus.readValues = {0xFF};
    Tas5805mDriver driver(bus, 0x2D);
    bool ok = driver.setModulationMode(
        Tas5805mModMode::Hybrid, Tas5805mSwFreq::Khz384, Tas5805mBdFreq::Khz100);
    ok = ok && bus.reads.size() == 1 &&
         bus.reads[0] == std::vector<uint8_t>{0x02};
    ok = ok && bus.writes.size() == 2 &&
         bus.writes[0] == std::vector<uint8_t>{0x02, 0x9E} &&
         bus.writes[1] == std::vector<uint8_t>{0x53, 0x20};
    BELL_LOG(info, LOG_TAG, "tas5805mDriverSmokeTest setModulationMode: {}",
             ok ? "PASS" : "FAIL");
    allOk = allOk && ok;
  }

  {
    // Every mixer-gain write is wrapped in its own book5/page0x29 enter and
    // book0/page0 restore (4 registers x (3 + 1 + 3) = 28 writes); only the
    // 5-byte (reg + 4-byte gain) writes carry the actual routing data.
    RecordingI2cBus bus;
    Tas5805mDriver driver(bus, 0x2D);
    bool ok = driver.setMixerMode(Tas5805mMixerMode::Stereo);
    std::vector<std::vector<uint8_t>> dataWrites;
    for (const auto& w : bus.writes) {
      if (w.size() == 5) {
        dataWrites.push_back(w);
      }
    }
    ok = ok && bus.writes.size() == 28 && dataWrites.size() == 4;
    if (ok) {
      auto reg = [](uint8_t r, uint32_t v) {
        std::vector<uint8_t> out{r};
        auto bytes = le32(v);
        out.insert(out.end(), bytes.begin(), bytes.end());
        return out;
      };
      ok = dataWrites[0] == reg(0x18, 0x00008000) &&
           dataWrites[1] == reg(0x24, 0x00008000) &&
           dataWrites[2] == reg(0x20, 0x00000000) &&
           dataWrites[3] == reg(0x1c, 0x00000000);
    }
    BELL_LOG(info, LOG_TAG, "tas5805mDriverSmokeTest setMixerMode: {}",
             ok ? "PASS" : "FAIL");
    allOk = allOk && ok;
  }

  {
    RecordingI2cBus bus;
    Tas5805mDriver driver(bus, 0x2D);
    bool ok = driver.setChannelGain(Tas5805mChannel::Right, 6);
    std::vector<std::vector<uint8_t>> dataWrites;
    for (const auto& w : bus.writes) {
      if (w.size() == 5) {
        dataWrites.push_back(w);
      }
    }
    ok = ok && bus.writes.size() == 7 && dataWrites.size() == 1 &&
         dataWrites[0][0] == 0x28;
    if (ok) {
      float linear = powf(10.0f, 6.0f / 20.0f);
      uint32_t expected = expectedQ9_23(linear);
      auto expectedBytes = le32(expected);
      ok = std::equal(dataWrites[0].begin() + 1, dataWrites[0].end(),
                      expectedBytes.begin());
    }
    BELL_LOG(info, LOG_TAG, "tas5805mDriverSmokeTest setChannelGain: {}",
             ok ? "PASS" : "FAIL");
    allOk = allOk && ok;
  }

  {
    RecordingI2cBus bus;
    bus.readValues = {0x05, 0x44, 0x01, 0x04};
    Tas5805mDriver driver(bus, 0x2D);
    Tas5805mFaults faults;
    bool ok = driver.getFaults(faults);
    ok = ok && faults.rightOverCurrent && faults.rightDcFault &&
         !faults.leftOverCurrent && !faults.leftDcFault;
    ok = ok && faults.clockFault && faults.biquadWriteFailed &&
         !faults.pvddUnderVoltage && !faults.pvddOverVoltage &&
         !faults.otpCrcError;
    ok = ok && faults.overTemperatureShutdown &&
         faults.overTemperatureWarning;
    ok = ok && faults.any();
    BELL_LOG(info, LOG_TAG, "tas5805mDriverSmokeTest getFaults: {}",
             ok ? "PASS" : "FAIL");
    allOk = allOk && ok;
  }

  {
    RecordingI2cBus bus;
    Tas5805mDriver driver(bus, 0x2D);
    bool ok = driver.clearFaults();
    ok = ok && bus.writes.size() == 1 &&
         bus.writes[0] == std::vector<uint8_t>{0x78, 0x80};
    BELL_LOG(info, LOG_TAG, "tas5805mDriverSmokeTest clearFaults: {}",
             ok ? "PASS" : "FAIL");
    allOk = allOk && ok;
  }

  {
    RecordingI2cBus bus;
    Tas5805mDriver driver(bus, 0x2D);
    bool ok = driver.setEqMode(Tas5805mEqMode::On);
    ok = ok && bus.writes.size() == 1 &&
         bus.writes[0] == std::vector<uint8_t>{0x66, 0b0110};
    BELL_LOG(info, LOG_TAG, "tas5805mDriverSmokeTest setEqMode: {}",
             ok ? "PASS" : "FAIL");
    allOk = allOk && ok;
  }

  {
    // tas5805m_eq_registers_left_mf[0..3] (gain step -15dB, band 0):
    // {0x24,0x18,0x07},{0x24,0x19,0xfc},{0x24,0x56,0xa2},{0x24,0x57,0x71}.
    RecordingI2cBus bus;
    Tas5805mDriver driver(bus, 0x2D);
    bool ok = driver.setEqGainChannel(Tas5805mChannel::Left, 0, -15);
    std::vector<std::vector<uint8_t>> dataWrites;
    for (const auto& w : bus.writes) {
      if (w.size() == 5) {
        dataWrites.push_back(w);
      }
    }
    ok = ok && bus.writes.size() == 11 && dataWrites.size() == 5 &&
         dataWrites[0] == std::vector<uint8_t>{0x18, 0x07, 0xfc, 0xa2, 0x71};
    BELL_LOG(info, LOG_TAG, "tas5805mDriverSmokeTest setEqGainChannel: {}",
             ok ? "PASS" : "FAIL");
    allOk = allOk && ok;
  }

  {
    // tas5805m_eq_registers_left_flat[0..3] (FLAT profile, band 0):
    // {0x24,0x18,0x08},{0x24,0x19,0x00},{0x24,0x1a,0x00},{0x24,0x1b,0x00}.
    RecordingI2cBus bus;
    Tas5805mDriver driver(bus, 0x2D);
    bool ok = driver.setEqProfileChannel(Tas5805mChannel::Left,
                                        Tas5805mEqProfile::Flat);
    std::vector<std::vector<uint8_t>> dataWrites;
    for (const auto& w : bus.writes) {
      if (w.size() == 5) {
        dataWrites.push_back(w);
      }
    }
    ok = ok && bus.writes.size() == 21 && dataWrites.size() == 15 &&
         dataWrites[0] == std::vector<uint8_t>{0x18, 0x08, 0x00, 0x00, 0x00};
    BELL_LOG(info, LOG_TAG, "tas5805mDriverSmokeTest setEqProfileChannel: {}",
             ok ? "PASS" : "FAIL");
    allOk = allOk && ok;
  }

  return allOk;
}

bool tas5805mSettingsSmokeTest() {
  namespace fs = std::filesystem;
  std::string path =
      (fs::temp_directory_path() / "snapclient_tas5805m_settings_smoketest.json")
          .string();
  std::error_code ec;
  fs::remove(path, ec);

  bool ok = true;

  {
    JsonFileSettingsStore store(path);
    Tas5805mSettings settings(store);
    ok = settings.applyJson(
             R"({"dac":{"analogGain":10,"dacMode":"pbtl",)"
             R"("modulation":{"mode":"hybrid","swFreqHz":384000,)"
             R"("bdFreqHz":100000},"mixerMode":"mono",)"
             R"("channelGainL":-6,"channelGainR":6,)"
             R"("eq":{"mode":"biamp",)"
             R"("gainL":[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15],)"
             R"("gainR":[-1,-2,-3,-4,-5,-6,-7,-8,-9,-10,-11,-12,-13,-14,-15],)"
             R"("profileL":"lf60","profileR":"hf90"}}})") &&
         ok;
    ok = (settings.analogGain() == 10) && ok;
    ok = (settings.dacMode() == Tas5805mDacMode::Pbtl) && ok;
    ok = (settings.modulationMode() == Tas5805mModMode::Hybrid) && ok;
    ok = (settings.swFreq() == Tas5805mSwFreq::Khz384) && ok;
    ok = (settings.bdFreq() == Tas5805mBdFreq::Khz100) && ok;
    ok = (settings.mixerMode() == Tas5805mMixerMode::Mono) && ok;
    ok = (settings.channelGainLeft() == -6) && ok;
    ok = (settings.channelGainRight() == 6) && ok;
    ok = (settings.eqMode() == Tas5805mEqMode::Biamp) && ok;
    ok = (settings.eqGainLeft()[0] == 1 && settings.eqGainLeft()[14] == 15) &&
         ok;
    ok = (settings.eqGainRight()[0] == -1 &&
         settings.eqGainRight()[14] == -15) &&
         ok;
    ok = (settings.eqProfileLeft() == Tas5805mEqProfile::Lf60) && ok;
    ok = (settings.eqProfileRight() == Tas5805mEqProfile::Hf90) && ok;
  }

  {
    // Fresh store/settings against the same file: verifies persistence
    // across reconstruction.
    JsonFileSettingsStore store(path);
    Tas5805mSettings settings(store);
    ok = (settings.analogGain() == 10) && ok;
    ok = (settings.dacMode() == Tas5805mDacMode::Pbtl) && ok;
    ok = (settings.mixerMode() == Tas5805mMixerMode::Mono) && ok;
    ok = (settings.channelGainLeft() == -6) && ok;
    ok = (settings.eqMode() == Tas5805mEqMode::Biamp) && ok;
    ok = (settings.eqGainLeft()[7] == 8) && ok;
    ok = (settings.eqGainRight()[7] == -8) && ok;
    ok = (settings.eqProfileLeft() == Tas5805mEqProfile::Lf60) && ok;
    ok = (settings.eqProfileRight() == Tas5805mEqProfile::Hf90) && ok;

    ok = !settings.applyJson(R"({"dac":{"analogGain":32}})") && ok;
    ok = !settings.applyJson(R"({"dac":{"dacMode":"nope"}})") && ok;
    ok = !settings.applyJson(R"({"dac":{"channelGainL":25}})") && ok;
    ok = !settings.applyJson(R"({"dac":{"eq":{"mode":"nope"}}})") && ok;
    ok = !settings.applyJson(
             R"({"dac":{"eq":{"gainL":[0,0,0,0,0,0,0,0,0,0,0,0,0,0]}}})") &&
         ok;
    ok = !settings.applyJson(
             R"({"dac":{"eq":{"gainL":)"
             R"([16,0,0,0,0,0,0,0,0,0,0,0,0,0,0]}}})") &&
         ok;
    ok = !settings.applyJson(R"({"dac":{"eq":{"profileL":"nope"}}})") && ok;
    ok = (settings.analogGain() == 10) && ok;
    ok = (settings.eqMode() == Tas5805mEqMode::Biamp) && ok;
  }

  fs::remove(path, ec);

  BELL_LOG(info, LOG_TAG, "tas5805mSettingsSmokeTest: {}",
           ok ? "PASS" : "FAIL");
  return ok;
}

bool udpLogBackendSmokeTest() {
  bell::UDPSocket listener;
  auto bindRes = listener.bind("127.0.0.1", 0, true);
  if (!bindRes) {
    BELL_LOG(error, LOG_TAG, "udpLogBackendSmokeTest: bind failed: {}",
             bindRes.error().message());
    return false;
  }
  auto port = static_cast<uint16_t>(*bindRes);

  auto backendRes = UdpLogBackend::create("127.0.0.1", port);
  if (!backendRes) {
    BELL_LOG(error, LOG_TAG, "udpLogBackendSmokeTest: create failed: {}",
             backendRes.error().message());
    return false;
  }

  (*backendRes)->log(bell::LogLevel::warn, "TestFile.cpp", 42, "testtag",
                     "hello world");

  std::array<std::byte, 512> buf{};
  bell::IpAddress fromAddr;
  auto recvRes = listener.recvfrom(buf.data(), buf.size(), fromAddr);
  if (!recvRes) {
    BELL_LOG(error, LOG_TAG, "udpLogBackendSmokeTest: recv failed: {}",
             recvRes.error().message());
    return false;
  }

  std::string received(reinterpret_cast<const char*>(buf.data()), *recvRes);
  bool ok = received.find("W [testtag] TestFile.cpp:42: hello world") !=
           std::string::npos;
  BELL_LOG(info, LOG_TAG, "udpLogBackendSmokeTest: {}", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace snapclient
