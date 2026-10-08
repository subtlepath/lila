#pragma once
#include <array>
#include <span>

#include "CompanionRecords.h"

namespace companion {
using PairingSecret = std::array<uint8_t, 32>;
using PairingPeer = std::array<uint8_t, 7>;
inline constexpr size_t MAX_PAIRINGS = 4;
inline constexpr size_t PAIRINGS_RECORD_SIZE = 232;
enum class PairingsRead { Present, Missing, Error };
class PairingsStorage {
 public:
  virtual ~PairingsStorage() = default;
  virtual PairingsRead read(std::span<uint8_t> bytes) = 0;
  // Must atomically commit and verify the complete record before success.
  virtual bool write(std::span<const uint8_t> bytes) = 0;
};
enum class PairingResult { Ok, Unavailable, Corrupt, Invalid, Exists, Full, IoError };
// Keep as a session member. No allocations; scratch is borrowed from the caller.
class Pairings {
 public:
  explicit Pairings(PairingsStorage& storage) : storage(storage) {}
  PairingResult load(std::span<uint8_t> scratch);
  PairingResult add(const Identity& installation, const PairingSecret& secret, const PairingPeer& peer,
                    std::span<uint8_t> scratch);
  PairingResult forget(const PairingPeer& peer, std::span<uint8_t> scratch);
  // Invoke only on an encrypted, authenticated BLE connection. Credentials
  // belong in the Apple installation's Keychain, never in synchronized records.
  bool boundTo(const Identity& installation, const PairingPeer& peer) const;
  bool authenticate(const Identity& installation, const PairingSecret& secret) const;

 private:
  struct Entry {
    bool used = false;
    Identity installation{};
    PairingSecret secret{};
    PairingPeer peer{};
  };
  void encode(std::span<uint8_t> bytes) const;
  PairingsStorage& storage;
  std::array<Entry, MAX_PAIRINGS> entries{};
  bool available = false;
};
}  // namespace companion
