#pragma once

#include "CompanionWifiHandoff.h"

namespace companion {
// Owns the global mDNS responder exclusively while the Wi-Fi session is active.
class HalCompanionWifiDiscovery final {
 public:
  HalCompanionWifiDiscovery() = default;
  ~HalCompanionWifiDiscovery();
  HalCompanionWifiDiscovery(const HalCompanionWifiDiscovery&) = delete;
  HalCompanionWifiDiscovery& operator=(const HalCompanionWifiDiscovery&) = delete;
  bool begin(const WifiHandoffOffer& offer);
  void end();

 private:
  bool active = false;
};
}  // namespace companion
