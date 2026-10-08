#pragma once

#include <CompanionPairings.h>
#include <CompanionWifiSecrets.h>

#include <cstdint>
#include <memory>
#include <span>

class HalCompanionBluetooth {
 public:
  enum class State : uint8_t { Off, Advertising, Connected, Authenticated, Error };
  struct Snapshot {
    State state = State::Off;
    uint32_t passkey = 0;
    bool showPasskey = false;
    bool operator==(const Snapshot&) const = default;
  };
  static constexpr char SERVICE_UUID[] = "e1b11000-7c43-4d94-86d6-c9c71ec60101";
  static constexpr char INPUT_UUID[] = "e1b11001-7c43-4d94-86d6-c9c71ec60101";
  static constexpr char OUTPUT_UUID[] = "e1b11002-7c43-4d94-86d6-c9c71ec60101";

  HalCompanionBluetooth();
  ~HalCompanionBluetooth();
  // Workspace belongs to the sync activity and must outlive stop(). The first
  // 5180 bytes hold the bounded queue and incoming fragment assembler.
  bool begin(std::span<uint8_t> workspace);
  void stop();
  Snapshot snapshot() const;
  size_t receive(std::span<uint8_t> destination, uint64_t& session);
  bool send(std::span<const uint8_t> frame, uint64_t session);
  bool peer(uint64_t session, companion::PairingPeer& output) const;
  uint64_t authenticatedSession() const;
  // Caller serializes radio lifecycle and separately authorizes the installation.
  bool generateWifiHandoffSecrets(uint64_t session, companion::Identity& identity, companion::Digest& key);
  bool generateWifiHotspotPassword(uint64_t session, companion::WifiHotspotPassword& password);
  bool unpairConnected(uint64_t session);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};
