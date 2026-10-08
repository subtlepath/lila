#pragma once

#include "../Serialization/BinaryRecordBytes.h"
#include "CompanionTintaBody.h"

namespace companion {
inline constexpr size_t TINTA_AUTHORITY_CHECKPOINT_SIZE = 80;
struct TintaAuthorityCheckpoint {
  Digest manifest{}, frontier{};
  uint32_t count = 0;
  uint16_t recordSize = 1024;
  bool operator==(const TintaAuthorityCheckpoint&) const = default;
};
inline bool validTintaAuthorityCheckpoint(const TintaAuthorityCheckpoint& value) {
  return tinta_body_detail::nonzero(value.manifest) && tinta_body_detail::nonzero(value.frontier) &&
         (value.recordSize == 512 || value.recordSize == 1024) && value.count <= UINT32_MAX / value.recordSize;
}
inline bool tintaAuthorityCheckpointOverlap(std::span<const uint8_t> bytes, const TintaAuthorityCheckpoint& value) {
  const auto first = reinterpret_cast<uintptr_t>(bytes.data()), second = reinterpret_cast<uintptr_t>(&value);
  return first >= second ? first - second < sizeof(value) : second - first < bytes.size();
}
inline bool encodeTintaAuthorityCheckpoint(const TintaAuthorityCheckpoint& value, std::span<uint8_t> output) {
  if (output.size() != TINTA_AUTHORITY_CHECKPOINT_SIZE || tintaAuthorityCheckpointOverlap(output, value) ||
      !validTintaAuthorityCheckpoint(value))
    return false;
  std::copy_n("TCP\1", 4, output.begin());
  std::copy(value.manifest.begin(), value.manifest.end(), output.begin() + 4);
  std::copy(value.frontier.begin(), value.frontier.end(), output.begin() + 36);
  binary_record::putU32(output.data() + 68, value.count);
  binary_record::putU16(output.data() + 72, value.recordSize);
  binary_record::putU16(output.data() + 74, 0);
  binary_record::putU32(output.data() + 76, binary_record::crc32(output.data(), 76));
  return true;
}
inline bool decodeTintaAuthorityCheckpoint(std::span<const uint8_t> input, TintaAuthorityCheckpoint& output) {
  if (input.size() != TINTA_AUTHORITY_CHECKPOINT_SIZE || tintaAuthorityCheckpointOverlap(input, output) ||
      !std::equal(input.begin(), input.begin() + 4, "TCP\1") || binary_record::getU16(input.data() + 74) != 0 ||
      binary_record::getU32(input.data() + 76) != binary_record::crc32(input.data(), 76))
    return false;
  TintaAuthorityCheckpoint value;
  std::copy_n(input.begin() + 4, 32, value.manifest.begin());
  std::copy_n(input.begin() + 36, 32, value.frontier.begin());
  value.count = binary_record::getU32(input.data() + 68);
  value.recordSize = binary_record::getU16(input.data() + 72);
  if (!validTintaAuthorityCheckpoint(value)) return false;
  output = value;
  return true;
}
// Caller independently hashes the full baseline manifest and audits current
// authority before computing its prefix digest using the checkpoint's count.
inline bool verifyTintaAuthorityCheckpoint(const TintaAuthorityCheckpoint& value, const Digest& manifestDigest,
                                           const Digest& baselineFrontier, uint32_t currentCount,
                                           uint16_t currentRecordSize, const Digest& prefixDigest) {
  return validTintaAuthorityCheckpoint(value) && value.manifest == manifestDigest &&
         value.frontier == baselineFrontier && value.frontier == prefixDigest && value.count <= currentCount &&
         (currentRecordSize == 512 || currentRecordSize == 1024) && currentRecordSize >= value.recordSize;
}
}  // namespace companion
