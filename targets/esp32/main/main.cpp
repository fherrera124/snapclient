#include <bell/Logger.h>

#include "snapclient/Core.h"

extern "C" void app_main(void) {
  bell::registerDefaultLogger();
  snapclient::scaffoldSelfCheck();
}
