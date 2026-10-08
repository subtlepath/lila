#pragma once

#include <string_view>

#include "CompanionFrame.h"
#include "CompanionWifiHandoff.h"
#include "CompanionWifiSecrets.h"

namespace companion {
enum class WifiNetworkMode : uint8_t { SavedNetwork = 1, Hotspot = 2 };
inline constexpr size_t WIFI_NETWORK_OFFER_HEADER_SIZE = 1 + WIFI_HANDOFF_OFFER_SIZE + 3;
inline constexpr size_t WIFI_NETWORK_OFFER_MAX_SIZE = WIFI_NETWORK_OFFER_HEADER_SIZE + 32 + 63;
static_assert(WIFI_NETWORK_OFFER_MAX_SIZE <= MAX_CONTROL_PAYLOAD);
struct WifiNetworkDescription {
  WifiNetworkMode mode = WifiNetworkMode::SavedNetwork;
  std::string_view ssid, password;
  bool operator==(const WifiNetworkDescription&) const = default;
};
// Strings borrow the wire buffer and expire before it is reused for Wi-Fi traffic.
struct WifiNetworkOffer {
  WifiHandoffOffer offer;
  WifiNetworkDescription network;
  bool operator==(const WifiNetworkOffer&) const = default;
};
static_assert(sizeof(WifiNetworkOffer) < 256);
inline bool validWifiNetworkDescription(const WifiNetworkDescription& network) {
  if (network.ssid.empty() || network.ssid.size() > 32 || network.ssid.find('\0') != std::string_view::npos)
    return false;
  if (network.mode == WifiNetworkMode::SavedNetwork) return network.password.empty();
  if (network.mode != WifiNetworkMode::Hotspot || network.password.size() < 8 || network.password.size() > 63)
    return false;
  const auto printable = [](std::string_view value) {
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) { return byte >= 32 && byte <= 126; });
  };
  return printable(network.ssid) && printable(network.password);
}
// Source strings must be disjoint from output. Keys/passwords travel only over authenticated BLE.
inline size_t encodeWifiNetworkOffer(const WifiNetworkOffer& value, std::span<uint8_t> output) {
  if (!validWifiNetworkDescription(value.network)) return 0;
  const size_t length = WIFI_NETWORK_OFFER_HEADER_SIZE + value.network.ssid.size() + value.network.password.size();
  if (output.size() < length || !validWifiHandoffOffer(value.offer)) return 0;
  output[0] = 1;
  encodeWifiHandoffOffer(value.offer, output.subspan(1, WIFI_HANDOFF_OFFER_SIZE));
  output[122] = static_cast<uint8_t>(value.network.mode);
  output[123] = static_cast<uint8_t>(value.network.ssid.size());
  output[124] = static_cast<uint8_t>(value.network.password.size());
  std::copy(value.network.ssid.begin(), value.network.ssid.end(), output.begin() + WIFI_NETWORK_OFFER_HEADER_SIZE);
  std::copy(value.network.password.begin(), value.network.password.end(),
            output.begin() + WIFI_NETWORK_OFFER_HEADER_SIZE + value.network.ssid.size());
  return length;
}
inline bool decodeWifiNetworkOffer(std::span<const uint8_t> input, WifiNetworkOffer& output) {
  if (input.size() < WIFI_NETWORK_OFFER_HEADER_SIZE || input.size() > WIFI_NETWORK_OFFER_MAX_SIZE || input[0] != 1)
    return false;
  const size_t ssidLength = input[123], passwordLength = input[124];
  if (input.size() != WIFI_NETWORK_OFFER_HEADER_SIZE + ssidLength + passwordLength) return false;
  struct Scratch {
    WifiNetworkOffer value;
    ~Scratch() { clearWifiHandoffSecrets(value.offer.session, value.offer.key); }
  } parsed;
  if (!decodeWifiHandoffOffer(input.subspan(1, WIFI_HANDOFF_OFFER_SIZE), parsed.value.offer)) return false;
  parsed.value.network.mode = static_cast<WifiNetworkMode>(input[122]);
  parsed.value.network.ssid = {reinterpret_cast<const char*>(input.data() + WIFI_NETWORK_OFFER_HEADER_SIZE),
                               ssidLength};
  parsed.value.network.password = {
      reinterpret_cast<const char*>(input.data() + WIFI_NETWORK_OFFER_HEADER_SIZE + ssidLength), passwordLength};
  if (!validWifiNetworkDescription(parsed.value.network)) return false;
  output = parsed.value;
  return true;
}
}  // namespace companion
