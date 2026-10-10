#pragma once

#include <Logging.h>

#include "HalCompanionBluetooth.h"

// Serialized consumer scope. Bluetooth and its workspace outlive this owner;
// borrowed command data must already be copied before construction.
class HalCompanionControlWorkspaceLease final {
 public:
  HalCompanionControlWorkspaceLease(HalCompanionBluetooth& bluetooth, uint64_t session)
      : session(session), bluetooth(bluetooth), held(bluetooth.acquireWorkspace(session)) {}
  ~HalCompanionControlWorkspaceLease() {
    if (held && !release()) LOG_ERR("COMPANION", "Control workspace release failed");
  }
  HalCompanionControlWorkspaceLease(const HalCompanionControlWorkspaceLease&) = delete;
  HalCompanionControlWorkspaceLease& operator=(const HalCompanionControlWorkspaceLease&) = delete;
  bool valid() const { return held && bluetooth.workspaceOwned(session); }
  bool release() {
    if (!held || !bluetooth.releaseWorkspace(session)) return false;
    held = false;
    return true;
  }
  static bool permitted(void* context) {
    return context && static_cast<HalCompanionControlWorkspaceLease*>(context)->valid();
  }

 private:
  uint64_t session;
  HalCompanionBluetooth& bluetooth;
  bool held;
};
