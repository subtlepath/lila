#pragma once

#include "../Serialization/BinaryRecordBytes.h"
#include "CompanionTintaDerivedManifest.h"

namespace companion {
inline constexpr size_t TINTA_RECEIVE_CHECKPOINT_SIZE = 132;
struct TintaReceiveCheckpoint {
  Identity course{}, transaction{}, owner{}, storage{};
  Digest manifestHash{};
  TintaDerivedFile file = TintaDerivedFile::Items;
  uint8_t sealedMask = 0;
  uint64_t offset = 0, length = 0, sequence = 0;
  bool operator==(const TintaReceiveCheckpoint&) const = default;
};
inline bool validTintaReceiveCheckpoint(const TintaReceiveCheckpoint& value) {
  for (const auto* id : {&value.course, &value.transaction, &value.owner, &value.storage})
    if (std::none_of(id->begin(), id->end(), [](uint8_t byte) { return byte != 0; })) return false;
  const auto index = static_cast<unsigned>(value.file);
  return index < 5 && value.sealedMask == static_cast<uint8_t>((1u << index) - 1) && value.sequence > 0 &&
         value.offset <= value.length && value.length <= UINT32_MAX &&
         std::any_of(value.manifestHash.begin(), value.manifestHash.end(), [](uint8_t byte) { return byte != 0; });
}
// expectedManifestHash must be computed from the exact immutable manifest bytes.
inline bool matchesTintaReceiveCheckpoint(const TintaReceiveCheckpoint& value, const TintaDerivedManifestView& manifest,
                                          const Digest& expectedManifestHash, const Identity& transaction,
                                          const Identity& owner, const Identity& course, const Identity& storage,
                                          const Digest& pack, const Digest& frontier) {
  return validTintaReceiveCheckpoint(value) && value.transaction == transaction && value.owner == owner &&
         value.course == course && value.storage == storage && value.manifestHash == expectedManifestHash &&
         manifest.matches(course, storage, pack, frontier) && value.length == manifest.length(value.file);
}
inline bool encodeTintaReceiveCheckpoint(const TintaReceiveCheckpoint& value, std::span<uint8_t> bytes) {
  if (bytes.size() != TINTA_RECEIVE_CHECKPOINT_SIZE || !validTintaReceiveCheckpoint(value)) return false;
  std::fill(bytes.begin(), bytes.end(), 0);
  bytes[0] = 'T';
  bytes[1] = 'R';
  bytes[2] = 'C';
  bytes[3] = '1';
  size_t at = 4;
  for (const auto* id : {&value.course, &value.transaction, &value.owner, &value.storage}) {
    std::copy(id->begin(), id->end(), bytes.begin() + at);
    at += 16;
  }
  std::copy(value.manifestHash.begin(), value.manifestHash.end(), bytes.begin() + 68);
  bytes[100] = static_cast<uint8_t>(value.file);
  bytes[101] = value.sealedMask;
  at = 104;
  for (const auto number : {value.offset, value.length, value.sequence}) {
    for (unsigned i = 0; i < 8; ++i) bytes[at + i] = static_cast<uint8_t>(number >> (8 * i));
    at += 8;
  }
  binary_record::putU32(bytes.data() + 128, binary_record::crc32(bytes.data(), 128));
  return true;
}
inline bool decodeTintaReceiveCheckpoint(std::span<const uint8_t> bytes, TintaReceiveCheckpoint& output) {
  if (bytes.size() != TINTA_RECEIVE_CHECKPOINT_SIZE || bytes[0] != 'T' || bytes[1] != 'R' || bytes[2] != 'C' ||
      bytes[3] != '1' || bytes[102] || bytes[103] ||
      binary_record::getU32(bytes.data() + 128) != binary_record::crc32(bytes.data(), 128))
    return false;
  TintaReceiveCheckpoint value;
  size_t at = 4;
  for (auto* id : {&value.course, &value.transaction, &value.owner, &value.storage}) {
    std::copy_n(bytes.begin() + at, 16, id->begin());
    at += 16;
  }
  std::copy_n(bytes.begin() + 68, 32, value.manifestHash.begin());
  value.file = static_cast<TintaDerivedFile>(bytes[100]);
  value.sealedMask = bytes[101];
  at = 104;
  for (auto* number : {&value.offset, &value.length, &value.sequence}) {
    for (unsigned i = 0; i < 8; ++i) *number |= static_cast<uint64_t>(bytes[at + i]) << (8 * i);
    at += 8;
  }
  if (!validTintaReceiveCheckpoint(value)) return false;
  output = value;
  return true;
}
}  // namespace companion
