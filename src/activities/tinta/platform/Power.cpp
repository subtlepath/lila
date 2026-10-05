#if LILA_TINTA

#include "platform/Power.h"

#include <Arduino.h>

namespace tinta::platform {

void Power::begin() {
  bootMs_ = millis();
  lastActivityMs_ = bootMs_;
}

void Power::noteActivity() { lastActivityMs_ = millis(); }

}  // namespace tinta::platform

#endif  // LILA_TINTA
