#pragma once

namespace snapclient {

void scaffoldSelfCheck();

// Runs synthetic PCM through every DspFlow; true if all passed.
bool dspSmokeTest();

// Round-trips JsonFileSettingsStore through a real file and exercises
// ControlSettings::applyJson's accept/reject paths; true if all passed.
bool settingsSmokeTest();

}  // namespace snapclient
