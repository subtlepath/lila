#pragma once

#include <esp_netif.h>
#include <esp_wifi.h>

#include <array>
#include <string_view>

#include "CompanionWifiNetworkOffer.h"

namespace companion {
// Exclusive Wi-Fi owner; BLE and other networking activities must be stopped.
// Keep outside the task stack because wifi_config_t holds the SDK credential union.
class HalCompanionWifiRadio final {
 public:
  HalCompanionWifiRadio() = default;
  ~HalCompanionWifiRadio();
  HalCompanionWifiRadio(const HalCompanionWifiRadio&) = delete;
  HalCompanionWifiRadio& operator=(const HalCompanionWifiRadio&) = delete;
  bool begin(WifiNetworkMode mode, std::string_view ssid, std::string_view password);
  bool address(std::array<uint8_t, 4>& output) const;
  // Call after ending HTTP/discovery. False forbids restarting BLE until cleanup succeeds.
  bool end();
  bool ownsResources() const { return initialized || netif || eventLoop; }

 private:
  bool fail(const char* operation, esp_err_t result);
  void clearConfiguration();
  wifi_config_t configuration{};
  esp_netif_t* netif = nullptr;
  bool initialized = false, started = false, attached = false, eventLoop = false;
};
}  // namespace companion
