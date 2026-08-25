#pragma once

namespace snapclient {

void scaffoldSelfCheck();

// Runs synthetic PCM through every DspFlow; true if all passed.
bool dspSmokeTest();

}  // namespace snapclient
