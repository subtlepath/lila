#include "CompanionIdentity.h"

#include <algorithm>
#include <array>
#include <limits>

namespace companion {
namespace {
bool nonzero(const Identity& value) {
  return std::any_of(value.begin(), value.end(), [](uint8_t byte) { return byte != 0; });
}
uint64_t number(std::span<const uint8_t> bytes) {
  uint64_t value = 0;
  for (size_t i = 0; i < bytes.size(); ++i) value |= uint64_t{bytes[i]} << (8 * i);
  return value;
}
void number(uint64_t value, std::span<uint8_t> bytes) {
  for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<uint8_t>(value >> (8 * i));
}
uint32_t crc(std::span<const uint8_t> bytes) {
  uint32_t value = 0xFFFFFFFF;
  for (uint8_t byte : bytes) {
    value ^= byte;
    for (unsigned i = 0; i < 8; ++i) value = (value >> 1) ^ (0xEDB88320U & (0U - (value & 1U)));
  }
  return ~value;
}
bool equal(const Identity& value, std::span<const uint8_t> bytes) {
  return std::equal(value.begin(), value.end(), bytes.begin());
}
void copy(const Identity& value, std::span<uint8_t> bytes) { std::copy(value.begin(), value.end(), bytes.begin()); }
}  // namespace

IdentityInspectionResult inspectIdentity(IdentityStorage& storage, IdentityState& output) {
  std::array<uint8_t, IDENTITY_RECORD_SIZE> record{};
  Identity device{}, card{}, marker{};
  if (!storage.hardwareIdentity(device) || !storage.cardIdentity(card)) return IdentityInspectionResult::IoError;
  if (!nonzero(device) || !nonzero(card)) return IdentityInspectionResult::Corrupt;
  const auto binding = storage.readBinding(record);
  if (binding == IdentityRead::Missing) return IdentityInspectionResult::Unavailable;
  if (binding == IdentityRead::Error) return IdentityInspectionResult::IoError;
  if (binding != IdentityRead::Present) return IdentityInspectionResult::Corrupt;
  const auto bytes = std::span<const uint8_t>(record);
  if (bytes[0] != 'L' || bytes[1] != 'C' || bytes[2] != 'I' || bytes[3] != 1 ||
      number(bytes.subspan(76, 4)) != crc(bytes.first(76)) ||
      !std::any_of(bytes.begin() + 52, bytes.begin() + 68, [](uint8_t byte) { return byte != 0; }))
    return IdentityInspectionResult::Corrupt;
  const auto epoch = number(bytes.subspan(68, 8));
  if (!epoch) return IdentityInspectionResult::Corrupt;
  if (!equal(device, bytes.subspan(4, 16))) return IdentityInspectionResult::WrongHardware;
  const auto markerStatus = storage.readMarker(marker);
  if (markerStatus == IdentityRead::Missing) return IdentityInspectionResult::Unavailable;
  if (markerStatus == IdentityRead::Error) return IdentityInspectionResult::IoError;
  if (markerStatus != IdentityRead::Present || !nonzero(marker)) return IdentityInspectionResult::Corrupt;
  if (!equal(card, bytes.subspan(20, 16)) || !equal(marker, bytes.subspan(36, 16)))
    return IdentityInspectionResult::WrongStorage;
  output.device = device;
  std::copy_n(bytes.begin() + 52, 16, output.storageGeneration.begin());
  output.eventEpoch = epoch;
  return IdentityInspectionResult::Ok;
}

IdentityResult provisionIdentity(IdentityStorage& storage, IdentityState& output) {
  std::array<uint8_t, IDENTITY_RECORD_SIZE> record{};
  IdentityState next;
  Identity card{};
  Identity marker{};
  if (!storage.hardwareIdentity(next.device) || !storage.cardIdentity(card)) return IdentityResult::IoError;
  if (!nonzero(next.device) || !nonzero(card)) return IdentityResult::Corrupt;
  const auto binding = storage.readBinding(record);
  if (binding == IdentityRead::Error) return IdentityResult::IoError;
  if (binding == IdentityRead::Corrupt) return IdentityResult::Corrupt;
  auto bytes = std::span(record);
  if (binding == IdentityRead::Present) {
    if (bytes[0] != 'L' || bytes[1] != 'C' || bytes[2] != 'I' || bytes[3] != 1 ||
        number(bytes.subspan(76, 4)) != crc(bytes.first(76)))
      return IdentityResult::Corrupt;
    if (!equal(next.device, bytes.subspan(4, 16))) return IdentityResult::WrongHardware;
    next.eventEpoch = number(bytes.subspan(68, 8));
    if (next.eventEpoch == 0) return IdentityResult::Corrupt;
    if (next.eventEpoch == std::numeric_limits<uint64_t>::max()) return IdentityResult::Exhausted;
    std::copy_n(bytes.begin() + 52, 16, next.storageGeneration.begin());
    if (!nonzero(next.storageGeneration)) return IdentityResult::Corrupt;
  }
  const auto markerStatus = storage.readMarker(marker);
  if (markerStatus == IdentityRead::Error) return IdentityResult::IoError;
  if (markerStatus == IdentityRead::Corrupt) return IdentityResult::Corrupt;
  if (markerStatus == IdentityRead::Missing) {
    if (!storage.randomIdentity(marker) || !nonzero(marker)) return IdentityResult::EntropyError;
    if (!storage.createMarker(marker)) return IdentityResult::IoError;
  } else if (!nonzero(marker))
    return IdentityResult::Corrupt;
  const bool changed =
      binding == IdentityRead::Missing || !equal(card, bytes.subspan(20, 16)) || !equal(marker, bytes.subspan(36, 16));
  if (changed) {
    const Identity previous = next.storageGeneration;
    if (!storage.randomIdentity(next.storageGeneration) || !nonzero(next.storageGeneration) ||
        next.storageGeneration == previous)
      return IdentityResult::EntropyError;
  }
  if (binding == IdentityRead::Missing) {
    // A factory reset must not restart the same hardware origin at epoch one.
    next.eventEpoch = number(std::span(next.storageGeneration).first(8)) & 0x7FFFFFFFFFFFFFFFULL;
    if (next.eventEpoch == 0) next.eventEpoch = 1;
  } else
    ++next.eventEpoch;
  bytes[0] = 'L';
  bytes[1] = 'C';
  bytes[2] = 'I';
  bytes[3] = 1;
  copy(next.device, bytes.subspan(4, 16));
  copy(card, bytes.subspan(20, 16));
  copy(marker, bytes.subspan(36, 16));
  copy(next.storageGeneration, bytes.subspan(52, 16));
  number(next.eventEpoch, bytes.subspan(68, 8));
  number(crc(bytes.first(76)), bytes.subspan(76, 4));
  if (!storage.writeBinding(bytes)) return IdentityResult::IoError;
  output = next;
  return IdentityResult::Ok;
}
}  // namespace companion
