#include <bell/Logger.h>

#include "snapclient/Core.h"

int main() {
  bell::registerDefaultLogger();
  snapclient::scaffoldSelfCheck();
  return 0;
}
