#pragma once

#include "CompanionContentRemovalRequest.h"
#include "CompanionInventoryIndex.h"

namespace companion {
enum class RemovalMetadataFile : uint8_t { State, Recent };
inline constexpr size_t REMOVAL_METADATA_SNAPSHOT_SIZE = 240;
inline constexpr uint64_t REMOVAL_METADATA_MAX_BYTES = 65536;
struct RemovalMetadataSnapshot {
  ContentRemovalRequest request;
  Digest planHash{}, previousHash{}, nextHash{};
  uint64_t previousLength = 0, nextLength = 0;
  RemovalMetadataFile file = RemovalMetadataFile::State;
  bool operator==(const RemovalMetadataSnapshot&) const = default;
};
static_assert(sizeof(RemovalMetadataSnapshot) < 256);
inline bool removalMetadataDigestNonzero(const Digest& hash) {
  return std::any_of(hash.begin(), hash.end(), [](uint8_t byte) { return byte != 0; });
}
inline bool validRemovalMetadataSnapshot(const RemovalMetadataSnapshot& value) {
  return validContentRemovalRequest(value.request) && removalMetadataDigestNonzero(value.planHash) &&
         removalMetadataDigestNonzero(value.nextHash) && value.nextLength &&
         value.nextLength <= REMOVAL_METADATA_MAX_BYTES && value.previousLength <= REMOVAL_METADATA_MAX_BYTES &&
         (removalMetadataDigestNonzero(value.previousHash) == (value.previousLength != 0)) &&
         (value.file == RemovalMetadataFile::State || value.file == RemovalMetadataFile::Recent);
}
inline bool unchangedRemovalMetadataSnapshot(const RemovalMetadataSnapshot& value) {
  return value.previousLength == value.nextLength && value.previousHash == value.nextHash;
}
inline size_t encodeRemovalMetadataSnapshot(const RemovalMetadataSnapshot& value, std::span<uint8_t> output) {
  if (output.size() < REMOVAL_METADATA_SNAPSHOT_SIZE || !validRemovalMetadataSnapshot(value)) return 0;
  static constexpr std::array<uint8_t, 8> PREFIX = {'L', 'R', 'M', 'S', 1, 0, 0, 0};
  std::copy(PREFIX.begin(), PREFIX.end(), output.begin());
  if (!encodeContentRemovalRequest(value.request, output.subspan(8, CONTENT_REMOVAL_REQUEST_SIZE))) return 0;
  std::copy(value.planHash.begin(), value.planHash.end(), output.begin() + 123);
  std::copy(value.previousHash.begin(), value.previousHash.end(), output.begin() + 155);
  std::copy(value.nextHash.begin(), value.nextHash.end(), output.begin() + 187);
  inventory_detail::write(output, 219, value.previousLength, 8);
  inventory_detail::write(output, 227, value.nextLength, 8);
  output[235] = static_cast<uint8_t>(value.file);
  inventory_detail::write(output, 236, inventoryIndexCrc(output.first(236)), 4);
  return REMOVAL_METADATA_SNAPSHOT_SIZE;
}
[[gnu::noinline]] inline bool decodeMetadataRemovalRequest(std::span<const uint8_t> bytes,
                                                           ContentRemovalRequest& output) {
  return decodeContentRemovalRequest(bytes, output);
}
[[gnu::noinline]] inline bool decodeRemovalMetadataSnapshot(std::span<const uint8_t> bytes,
                                                            RemovalMetadataSnapshot& output) {
  static constexpr std::array<uint8_t, 8> PREFIX = {'L', 'R', 'M', 'S', 1, 0, 0, 0};
  const auto input = reinterpret_cast<uintptr_t>(bytes.data());
  const auto destination = reinterpret_cast<uintptr_t>(&output);
  const bool disjoint =
      input <= destination ? bytes.size() <= destination - input : sizeof(output) <= input - destination;
  if (!disjoint || bytes.size() != REMOVAL_METADATA_SNAPSHOT_SIZE ||
      !std::equal(PREFIX.begin(), PREFIX.end(), bytes.begin()) ||
      inventory_detail::read(bytes, 236, 4) != inventoryIndexCrc(bytes.first(236)))
    return false;
  ContentRemovalRequest request;
  if (!decodeMetadataRemovalRequest(bytes.subspan(8, CONTENT_REMOVAL_REQUEST_SIZE), request)) return false;
  const auto nonzero = [](std::span<const uint8_t> hash) {
    return std::any_of(hash.begin(), hash.end(), [](uint8_t byte) { return byte != 0; });
  };
  const uint64_t previous = inventory_detail::read(bytes, 219, 8);
  const uint64_t next = inventory_detail::read(bytes, 227, 8);
  const auto kind = static_cast<RemovalMetadataFile>(bytes[235]);
  if (!nonzero(bytes.subspan(123, 32)) || !nonzero(bytes.subspan(187, 32)) || !next ||
      next > REMOVAL_METADATA_MAX_BYTES || previous > REMOVAL_METADATA_MAX_BYTES ||
      nonzero(bytes.subspan(155, 32)) != (previous != 0) ||
      (kind != RemovalMetadataFile::State && kind != RemovalMetadataFile::Recent))
    return false;
  output.request = request;
  std::copy_n(bytes.begin() + 123, 32, output.planHash.begin());
  std::copy_n(bytes.begin() + 155, 32, output.previousHash.begin());
  std::copy_n(bytes.begin() + 187, 32, output.nextHash.begin());
  output.previousLength = previous;
  output.nextLength = next;
  output.file = kind;
  return true;
}
}  // namespace companion
