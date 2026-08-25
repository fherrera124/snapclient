#include "snapclient/Core.h"

#include <bell/Logger.h>

namespace snapclient {

static const char* LOG_TAG = "snapclient";

void scaffoldSelfCheck() {
  BELL_LOG(info, LOG_TAG, "snapclient_core linked against bell, scaffold ok");
}

}  // namespace snapclient
