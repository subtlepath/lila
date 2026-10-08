#pragma once

#include "CompanionWifiNetworkOffer.h"

namespace companion {
enum class WifiHandoffAction : uint8_t { Prepare = 1, Activate = 2, Cancel = 3 };
inline constexpr size_t WIFI_HANDOFF_PREPARE_SIZE = 19;
inline constexpr size_t WIFI_HANDOFF_SESSION_COMMAND_SIZE = 34;
struct WifiHandoffPrepare {
  WifiNetworkMode mode = WifiNetworkMode::SavedNetwork;
  Identity transaction{};
  bool operator==(const WifiHandoffPrepare&) const = default;
};
struct WifiHandoffSessionCommand {
  WifiHandoffAction action = WifiHandoffAction::Activate;
  Identity transaction{}, session{};
  bool operator==(const WifiHandoffSessionCommand&) const = default;
};
inline bool nonzeroWifiIdentity(const Identity& value) {
  return std::any_of(value.begin(), value.end(), [](uint8_t byte) { return byte != 0; });
}
inline size_t encodeWifiHandoffPrepare(const WifiHandoffPrepare& value, std::span<uint8_t> output) {
  if (output.size() < WIFI_HANDOFF_PREPARE_SIZE || !nonzeroWifiIdentity(value.transaction) ||
      (value.mode != WifiNetworkMode::SavedNetwork && value.mode != WifiNetworkMode::Hotspot))
    return 0;
  output[0] = 1;
  output[1] = static_cast<uint8_t>(WifiHandoffAction::Prepare);
  output[2] = static_cast<uint8_t>(value.mode);
  std::copy(value.transaction.begin(), value.transaction.end(), output.begin() + 3);
  return WIFI_HANDOFF_PREPARE_SIZE;
}
inline bool decodeWifiHandoffPrepare(std::span<const uint8_t> input, WifiHandoffPrepare& output) {
  if (input.size() != WIFI_HANDOFF_PREPARE_SIZE || input[0] != 1 ||
      input[1] != static_cast<uint8_t>(WifiHandoffAction::Prepare))
    return false;
  WifiHandoffPrepare parsed;
  parsed.mode = static_cast<WifiNetworkMode>(input[2]);
  std::copy_n(input.begin() + 3, parsed.transaction.size(), parsed.transaction.begin());
  if (!nonzeroWifiIdentity(parsed.transaction) ||
      (parsed.mode != WifiNetworkMode::SavedNetwork && parsed.mode != WifiNetworkMode::Hotspot))
    return false;
  output = parsed;
  return true;
}
inline size_t encodeWifiHandoffSessionCommand(const WifiHandoffSessionCommand& value, std::span<uint8_t> output) {
  if (output.size() < WIFI_HANDOFF_SESSION_COMMAND_SIZE || !nonzeroWifiIdentity(value.transaction) ||
      !nonzeroWifiIdentity(value.session) ||
      (value.action != WifiHandoffAction::Activate && value.action != WifiHandoffAction::Cancel))
    return 0;
  output[0] = 1;
  output[1] = static_cast<uint8_t>(value.action);
  std::copy(value.transaction.begin(), value.transaction.end(), output.begin() + 2);
  std::copy(value.session.begin(), value.session.end(), output.begin() + 18);
  return WIFI_HANDOFF_SESSION_COMMAND_SIZE;
}
inline bool decodeWifiHandoffSessionCommand(std::span<const uint8_t> input, WifiHandoffSessionCommand& output) {
  if (input.size() != WIFI_HANDOFF_SESSION_COMMAND_SIZE || input[0] != 1) return false;
  WifiHandoffSessionCommand parsed;
  parsed.action = static_cast<WifiHandoffAction>(input[1]);
  if (parsed.action != WifiHandoffAction::Activate && parsed.action != WifiHandoffAction::Cancel) return false;
  std::copy_n(input.begin() + 2, parsed.transaction.size(), parsed.transaction.begin());
  std::copy_n(input.begin() + 18, parsed.session.size(), parsed.session.begin());
  if (!nonzeroWifiIdentity(parsed.transaction) || !nonzeroWifiIdentity(parsed.session)) return false;
  output = parsed;
  return true;
}
}  // namespace companion
