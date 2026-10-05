#pragma once

// Activity timing. Inside lila, sleep and power belong to lila: its idle timer
// and Power key put the device to sleep, after the activity's onExit() has
// written everything Tinta holds (App::close()).

#include <stdint.h>

namespace tinta::platform {

class Power {
 public:
  // When the app opens, so bootMs() is the start of this visit.
  void begin();

  uint32_t bootMs() const { return bootMs_; }
  void noteActivity();
  uint32_t lastActivityMs() const { return lastActivityMs_; }

 private:
  uint32_t bootMs_ = 0;
  uint32_t lastActivityMs_ = 0;
};

}  // namespace tinta::platform
