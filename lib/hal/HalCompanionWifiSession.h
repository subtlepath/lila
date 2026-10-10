#pragma once

#include "HalCompanionWifiDiscovery.h"
#include "HalCompanionWifiHttp.h"

namespace companion {
// Activity-owned outside the task stack. Wi-Fi must be ready and BLE stopped.
// Owns the global mDNS responder exclusively; end before stopping Wi-Fi.
class HalCompanionWifiSession final {
 public:
  HalCompanionWifiSession() = default;
  ~HalCompanionWifiSession();
  HalCompanionWifiSession(const HalCompanionWifiSession&) = delete;
  HalCompanionWifiSession& operator=(const HalCompanionWifiSession&) = delete;
  bool begin(const WifiHandoffOffer& offer, const Identity& reader, const Identity& generation,
             const Identity& installation, const Identity& transaction, std::span<uint8_t> workspace,
             uint64_t receivedAtMilliseconds, WifiMessageClock clock, WifiMessageDispatch dispatch);
  // False means all session resources are released; the radio owner restores BLE.
  bool poll();
  void end();
  bool isActive() const { return active; }
  bool acquireWorkspace(const Identity& session) { return active && polling && messages.acquireWorkspace(session); }
  bool workspaceOwned(const Identity& session) const { return active && polling && messages.workspaceOwned(session); }
  bool releaseWorkspace(const Identity& session) { return messages.releaseWorkspace(session); }

 private:
  HalCompanionWifiMessages messages;
  HalCompanionWifiHttp http{messages};
  HalCompanionWifiDiscovery discovery;
  bool active = false, polling = false, starting = false, startupCancelled = false;
};
}  // namespace companion
