#pragma once

#include <algorithm>

#include "CompanionRecords.h"

namespace companion {
inline constexpr size_t WIFI_HANDOFF_OFFER_SIZE = 121;
inline constexpr uint16_t WIFI_HANDOFF_MAX_LIFETIME_SECONDS = 120;
inline constexpr size_t WIFI_HANDOFF_HOSTNAME_SIZE = 38;
// Caller supplies storage; names contain only the ephemeral session identity.
inline bool wifiHandoffHostname(const Identity& session, std::span<char> output) {
  if (output.size() < WIFI_HANDOFF_HOSTNAME_SIZE ||
      std::none_of(session.begin(), session.end(), [](uint8_t byte) { return byte != 0; }))
    return false;
  static constexpr char PREFIX[] = "lila-";
  static constexpr char HEX_DIGITS[] = "0123456789abcdef";
  std::copy_n(PREFIX, 5, output.begin());
  size_t at = 5;
  for (const auto byte : session) {
    output[at++] = HEX_DIGITS[byte >> 4];
    output[at++] = HEX_DIGITS[byte & 15];
  }
  output[at] = '\0';
  return true;
}

// Ephemeral secrets: deliver only over authenticated BLE; do not persist or log.
struct WifiHandoffOffer {
  Identity reader{}, storageGeneration{}, installation{}, transaction{}, session{};
  Digest key{};
  std::array<uint8_t, 4> address{};
  uint16_t port = 0, lifetimeSeconds = 0;
  bool operator==(const WifiHandoffOffer&) const = default;
};
static_assert(sizeof(WifiHandoffOffer) < 256);
inline bool validWifiHandoffOffer(const WifiHandoffOffer& offer) {
  const auto nonzero = [](const auto& bytes) {
    return std::any_of(bytes.begin(), bytes.end(), [](uint8_t byte) { return byte != 0; });
  };
  return nonzero(offer.reader) && nonzero(offer.storageGeneration) && nonzero(offer.installation) &&
         nonzero(offer.transaction) && nonzero(offer.session) && nonzero(offer.key) &&
         (!nonzero(offer.address) || (offer.address[0] != 0 && offer.address[0] != 127 && offer.address[0] < 224)) &&
         offer.port != 0 && offer.lifetimeSeconds != 0 && offer.lifetimeSeconds <= WIFI_HANDOFF_MAX_LIFETIME_SECONDS;
}
inline size_t encodeWifiHandoffOffer(const WifiHandoffOffer& offer, std::span<uint8_t> output) {
  if (!validWifiHandoffOffer(offer) || output.size() < WIFI_HANDOFF_OFFER_SIZE) return 0;
  output[0] = 1;
  size_t offset = 1;
  for (const auto* identity :
       {&offer.reader, &offer.storageGeneration, &offer.installation, &offer.transaction, &offer.session}) {
    std::copy(identity->begin(), identity->end(), output.begin() + offset);
    offset += identity->size();
  }
  std::copy(offer.key.begin(), offer.key.end(), output.begin() + offset);
  offset += offer.key.size();
  std::copy(offer.address.begin(), offer.address.end(), output.begin() + offset);
  offset += offer.address.size();
  output[offset] = static_cast<uint8_t>(offer.port);
  output[offset + 1] = static_cast<uint8_t>(offer.port >> 8);
  output[offset + 2] = static_cast<uint8_t>(offer.lifetimeSeconds);
  output[offset + 3] = static_cast<uint8_t>(offer.lifetimeSeconds >> 8);
  return WIFI_HANDOFF_OFFER_SIZE;
}
inline bool decodeWifiHandoffOffer(std::span<const uint8_t> input, WifiHandoffOffer& output) {
  if (input.size() != WIFI_HANDOFF_OFFER_SIZE || input[0] != 1) return false;
  WifiHandoffOffer parsed;
  size_t offset = 1;
  for (auto* identity :
       {&parsed.reader, &parsed.storageGeneration, &parsed.installation, &parsed.transaction, &parsed.session}) {
    std::copy_n(input.begin() + offset, identity->size(), identity->begin());
    offset += identity->size();
  }
  std::copy_n(input.begin() + offset, parsed.key.size(), parsed.key.begin());
  offset += parsed.key.size();
  std::copy_n(input.begin() + offset, parsed.address.size(), parsed.address.begin());
  offset += parsed.address.size();
  parsed.port = input[offset] | (static_cast<uint16_t>(input[offset + 1]) << 8);
  parsed.lifetimeSeconds = input[offset + 2] | (static_cast<uint16_t>(input[offset + 3]) << 8);
  if (!validWifiHandoffOffer(parsed)) return false;
  output = parsed;
  return true;
}
inline bool wifiHandoffMatches(const WifiHandoffOffer& offer, const Identity& reader, const Identity& generation,
                               const Identity& installation, const Identity& transaction) {
  return validWifiHandoffOffer(offer) && offer.reader == reader && offer.storageGeneration == generation &&
         offer.installation == installation && offer.transaction == transaction;
}
}  // namespace companion
