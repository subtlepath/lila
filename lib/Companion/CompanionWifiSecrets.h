#pragma once

#include <algorithm>
#include <array>
#include <span>

#include "CompanionRecords.h"

namespace companion {
inline void clearWifiHandoffSecrets(Identity& session, Digest& key) {
  volatile uint8_t* secret = key.data();
  for (size_t at = 0; at < key.size(); ++at) secret[at] = 0;
  volatile uint8_t* identity = session.data();
  for (size_t at = 0; at < session.size(); ++at) identity[at] = 0;
}
struct WifiRandomSource {
  void* context = nullptr;
  bool (*fill)(void*, std::span<uint8_t>) = nullptr;
};
inline constexpr size_t WIFI_HOTSPOT_PASSWORD_LENGTH = 32;
using WifiHotspotPassword = std::array<char, WIFI_HOTSPOT_PASSWORD_LENGTH + 1>;
inline void clearWifiHotspotPassword(WifiHotspotPassword& password) {
  volatile char* secret = password.data();
  for (size_t at = 0; at < password.size(); ++at) secret[at] = 0;
}
// Use a separate entropy draw, never bytes from the application encryption key.
inline bool generateWifiHotspotPassword(WifiHotspotPassword& password, WifiRandomSource random) {
  clearWifiHotspotPassword(password);
  if (!random.fill) return false;
  struct Scratch {
    std::array<uint8_t, 16> bytes{};
    ~Scratch() {
      volatile uint8_t* secret = bytes.data();
      for (size_t at = 0; at < bytes.size(); ++at) secret[at] = 0;
    }
  } scratch;
  if (!random.fill(random.context, scratch.bytes) ||
      !std::any_of(scratch.bytes.begin(), scratch.bytes.end(), [](uint8_t byte) { return byte != 0; }))
    return false;
  static constexpr char HEX_DIGITS[] = "0123456789abcdef";
  for (size_t at = 0; at < scratch.bytes.size(); ++at) {
    password[at * 2] = HEX_DIGITS[scratch.bytes[at] >> 4];
    password[at * 2 + 1] = HEX_DIGITS[scratch.bytes[at] & 15];
  }
  return true;
}
// The caller must establish a cryptographic entropy source before invoking this.
inline bool generateWifiHandoffSecrets(Identity& session, Digest& key, WifiRandomSource random) {
  clearWifiHandoffSecrets(session, key);
  if (!random.fill) return false;
  struct Scratch {
    std::array<uint8_t, 48> bytes{};
    ~Scratch() {
      volatile uint8_t* secret = bytes.data();
      for (size_t at = 0; at < bytes.size(); ++at) secret[at] = 0;
    }
  } scratch;
  if (!random.fill(random.context, scratch.bytes)) return false;
  const auto generatedSession = std::span(scratch.bytes).first(session.size());
  const auto generatedKey = std::span(scratch.bytes).subspan(session.size());
  const auto nonzero = [](const auto& bytes) {
    return std::any_of(bytes.begin(), bytes.end(), [](uint8_t byte) { return byte != 0; });
  };
  if (!nonzero(generatedSession) || !nonzero(generatedKey)) return false;
  std::copy(generatedSession.begin(), generatedSession.end(), session.begin());
  std::copy(generatedKey.begin(), generatedKey.end(), key.begin());
  return true;
}
}  // namespace companion
