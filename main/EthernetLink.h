#pragma once

namespace snapclient {

// Brings up whatever Ethernet interfaces Kconfig configured and attaches
// them to the TCP/IP stack. False when none is configured or none came up;
// the caller carries on with WiFi either way.
bool ethernetStart();

}  // namespace snapclient
