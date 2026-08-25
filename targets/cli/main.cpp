#include <bell/Logger.h>

#include "snapclient/Core.h"

int main() {
  bell::registerDefaultLogger();
  snapclient::scaffoldSelfCheck();
  return snapclient::dspSmokeTest() ? 0 : 1;
}
