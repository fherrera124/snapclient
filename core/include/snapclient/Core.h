#pragma once

namespace snapclient {

void scaffoldSelfCheck();

// Runs synthetic PCM through every DspFlow; true if all passed.
bool dspSmokeTest();

// Round-trips JsonFileSettingsStore through a real file and exercises
// ControlSettings::applyJson's accept/reject paths; true if all passed.
bool settingsSmokeTest();

// Drives Tas5805mDriver over a recording fake I2cBus and asserts the
// resulting byte sequences against expected register/book-page writes;
// true if all passed. No real chip involved.
bool tas5805mDriverSmokeTest();

// Round-trips Tas5805mSettings through a real file, mirroring
// settingsSmokeTest(); true if all passed.
bool tas5805mSettingsSmokeTest();

// Binds a real loopback UDP socket, sends a log line through
// UdpLogBackend, and asserts the exact bytes received; true if passed.
bool udpLogBackendSmokeTest();

}  // namespace snapclient
