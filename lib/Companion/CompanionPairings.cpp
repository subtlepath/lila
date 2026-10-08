#include "CompanionPairings.h"

#include <algorithm>
namespace companion {
namespace {
constexpr size_t ENTRY_SIZE = 56;
uint32_t checksum(std::span<const uint8_t> bytes) {
  uint32_t crc = 0xffffffff;
  for (uint8_t byte : bytes) {
    crc ^= byte;
    for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
  }
  return ~crc;
}
void seal(std::span<uint8_t> bytes) {
  const uint32_t crc = checksum(bytes.first(PAIRINGS_RECORD_SIZE - 4));
  for (unsigned i = 0; i < 4; ++i) bytes[PAIRINGS_RECORD_SIZE - 4 + i] = static_cast<uint8_t>(crc >> (8 * i));
}
bool nonzero(std::span<const uint8_t> bytes) {
  return std::any_of(bytes.begin(), bytes.end(), [](uint8_t b) { return b != 0; });
}
}  // namespace
void Pairings::encode(std::span<uint8_t> bytes) const {
  bytes = bytes.first(PAIRINGS_RECORD_SIZE);
  std::fill(bytes.begin(), bytes.end(), 0);
  bytes[0] = 'L';
  bytes[1] = 'C';
  bytes[2] = 'P';
  bytes[3] = 1;
  for (size_t i = 0; i < entries.size(); ++i) {
    if (!entries[i].used) continue;
    auto slot = bytes.subspan(4 + i * ENTRY_SIZE, ENTRY_SIZE);
    slot[0] = 1;
    std::copy(entries[i].installation.begin(), entries[i].installation.end(), slot.begin() + 1);
    std::copy(entries[i].secret.begin(), entries[i].secret.end(), slot.begin() + 17);
    std::copy(entries[i].peer.begin(), entries[i].peer.end(), slot.begin() + 49);
  }
  seal(bytes);
}
PairingResult Pairings::load(std::span<uint8_t> scratch) {
  available = false;
  if (scratch.size() < PAIRINGS_RECORD_SIZE) return PairingResult::Invalid;
  auto bytes = scratch.first(PAIRINGS_RECORD_SIZE);
  const auto result = storage.read(bytes);
  if (result == PairingsRead::Error) return PairingResult::IoError;
  if (result == PairingsRead::Missing) {
    entries = {};
    available = true;
    return PairingResult::Ok;
  }
  uint32_t expected = 0;
  for (unsigned i = 0; i < 4; ++i) expected |= static_cast<uint32_t>(bytes[PAIRINGS_RECORD_SIZE - 4 + i]) << (8 * i);
  if (bytes[0] != 'L' || bytes[1] != 'C' || bytes[2] != 'P' || bytes[3] != 1 ||
      checksum(bytes.first(PAIRINGS_RECORD_SIZE - 4)) != expected)
    return PairingResult::Corrupt;
  // Validate the whole record before replacing session state.
  for (size_t i = 0; i < entries.size(); ++i) {
    const auto slot = bytes.subspan(4 + i * ENTRY_SIZE, ENTRY_SIZE);
    if (slot[0] > 1 || (!slot[0] && nonzero(slot.subspan(1))) ||
        (slot[0] && (!nonzero(slot.subspan(1, 16)) || !nonzero(slot.subspan(17, 32)))))
      return PairingResult::Corrupt;
    if (slot[0])
      for (size_t j = 0; j < i; ++j) {
        const auto other = bytes.subspan(4 + j * ENTRY_SIZE, ENTRY_SIZE);
        if (other[0] && std::equal(slot.begin() + 1, slot.begin() + 17, other.begin() + 1))
          return PairingResult::Corrupt;
      }
  }
  for (size_t i = 0; i < entries.size(); ++i) {
    const auto slot = bytes.subspan(4 + i * ENTRY_SIZE, ENTRY_SIZE);
    entries[i].used = slot[0];
    std::copy_n(slot.begin() + 1, 16, entries[i].installation.begin());
    std::copy_n(slot.begin() + 17, 32, entries[i].secret.begin());
    std::copy_n(slot.begin() + 49, 7, entries[i].peer.begin());
  }
  available = true;
  return PairingResult::Ok;
}
PairingResult Pairings::add(const Identity& installation, const PairingSecret& secret, const PairingPeer& peer,
                            std::span<uint8_t> scratch) {
  if (!available) return PairingResult::Unavailable;
  if (scratch.size() < PAIRINGS_RECORD_SIZE || !nonzero(installation) || !nonzero(secret))
    return PairingResult::Invalid;
  size_t free = entries.size();
  for (size_t i = 0; i < entries.size(); ++i) {
    if (entries[i].used && entries[i].installation == installation) return PairingResult::Exists;
    if (!entries[i].used && free == entries.size()) free = i;
  }
  if (free == entries.size()) return PairingResult::Full;
  encode(scratch);
  auto slot = scratch.subspan(4 + free * ENTRY_SIZE, ENTRY_SIZE);
  slot[0] = 1;
  std::copy(installation.begin(), installation.end(), slot.begin() + 1);
  std::copy(secret.begin(), secret.end(), slot.begin() + 17);
  std::copy(peer.begin(), peer.end(), slot.begin() + 49);
  seal(scratch.first(PAIRINGS_RECORD_SIZE));
  if (!storage.write(scratch.first(PAIRINGS_RECORD_SIZE))) {
    available = false;
    return PairingResult::IoError;
  }
  entries[free] = {true, installation, secret, peer};
  return PairingResult::Ok;
}
PairingResult Pairings::forget(const PairingPeer& peer, std::span<uint8_t> scratch) {
  if (!available) return PairingResult::Unavailable;
  if (scratch.size() < PAIRINGS_RECORD_SIZE) return PairingResult::Invalid;
  bool found = false;
  encode(scratch);
  for (size_t i = 0; i < entries.size(); ++i)
    if (entries[i].used && entries[i].peer == peer) {
      found = true;
      auto slot = scratch.subspan(4 + i * ENTRY_SIZE, ENTRY_SIZE);
      std::fill(slot.begin(), slot.end(), 0);
    }
  if (!found) return PairingResult::Ok;
  seal(scratch.first(PAIRINGS_RECORD_SIZE));
  if (!storage.write(scratch.first(PAIRINGS_RECORD_SIZE))) {
    available = false;
    return PairingResult::IoError;
  }
  for (auto& entry : entries)
    if (entry.used && entry.peer == peer) entry = {};
  return PairingResult::Ok;
}
bool Pairings::boundTo(const Identity& installation, const PairingPeer& peer) const {
  if (!available) return false;
  for (const auto& entry : entries)
    if (entry.used && entry.installation == installation && entry.peer == peer) return true;
  return false;
}
bool Pairings::authenticate(const Identity& installation, const PairingSecret& secret) const {
  if (!available) return false;
  uint8_t accepted = 0;
  for (const auto& entry : entries) {
    uint8_t difference = 0;
    for (size_t i = 0; i < secret.size(); ++i) difference |= entry.secret[i] ^ secret[i];
    accepted |= entry.used && entry.installation == installation && difference == 0;
  }
  return accepted != 0;
}
}  // namespace companion
