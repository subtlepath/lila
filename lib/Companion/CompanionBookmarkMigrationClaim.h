#pragma once

#include "CompanionBookmarkPublicationRecord.h"

namespace companion {
struct BookmarkMigrationClaim {
  Identity transaction{}, storageGeneration{};
  Digest edition{}, originalHash{}, sourcePathHash{};
  uint64_t originalLength = 0;
  bool operator==(const BookmarkMigrationClaim&) const = default;
};
inline constexpr size_t BOOKMARK_MIGRATION_CLAIM_SIZE = 148;
inline bool validBookmarkMigrationClaim(const BookmarkMigrationClaim& claim) {
  return tinta_body_detail::nonzero(claim.transaction) && tinta_body_detail::nonzero(claim.storageGeneration) &&
         tinta_body_detail::nonzero(claim.edition) && tinta_body_detail::nonzero(claim.originalHash) &&
         tinta_body_detail::nonzero(claim.sourcePathHash) && claim.originalLength && claim.originalLength <= UINT32_MAX;
}
inline bool encodeBookmarkMigrationClaim(const BookmarkMigrationClaim& claim, std::span<uint8_t> bytes) {
  if (bytes.size() != BOOKMARK_MIGRATION_CLAIM_SIZE || !validBookmarkMigrationClaim(claim)) return false;
  std::fill(bytes.begin(), bytes.end(), 0);
  bytes[0] = 'B';
  bytes[1] = 'M';
  bytes[2] = 'L';
  bytes[3] = 'C';
  bytes[4] = 1;
  std::copy(claim.transaction.begin(), claim.transaction.end(), bytes.begin() + 8);
  std::copy(claim.storageGeneration.begin(), claim.storageGeneration.end(), bytes.begin() + 24);
  std::copy(claim.edition.begin(), claim.edition.end(), bytes.begin() + 40);
  std::copy(claim.originalHash.begin(), claim.originalHash.end(), bytes.begin() + 72);
  std::copy(claim.sourcePathHash.begin(), claim.sourcePathHash.end(), bytes.begin() + 104);
  bookmark_record_detail::put(bytes.subspan(136, 8), claim.originalLength);
  bookmark_record_detail::put(bytes.last(4), bookmark_record_detail::checksum(bytes.first(144)));
  return true;
}
inline bool decodeBookmarkMigrationClaim(std::span<const uint8_t> bytes, BookmarkMigrationClaim& output) {
  if (bytes.size() != BOOKMARK_MIGRATION_CLAIM_SIZE || bytes[0] != 'B' || bytes[1] != 'M' || bytes[2] != 'L' ||
      bytes[3] != 'C' || bytes[4] != 1 || bytes[5] || bytes[6] || bytes[7] ||
      bookmark_record_detail::get(bytes.last(4)) != bookmark_record_detail::checksum(bytes.first(144)) ||
      !tinta_body_detail::nonzero(bytes.subspan(8, 16)) || !tinta_body_detail::nonzero(bytes.subspan(24, 16)) ||
      !tinta_body_detail::nonzero(bytes.subspan(40, 32)) || !tinta_body_detail::nonzero(bytes.subspan(72, 32)) ||
      !tinta_body_detail::nonzero(bytes.subspan(104, 32)))
    return false;
  const auto length = bookmark_record_detail::get(bytes.subspan(136, 8));
  if (!length || length > UINT32_MAX) return false;
  std::copy_n(bytes.begin() + 8, 16, output.transaction.begin());
  std::copy_n(bytes.begin() + 24, 16, output.storageGeneration.begin());
  std::copy_n(bytes.begin() + 40, 32, output.edition.begin());
  std::copy_n(bytes.begin() + 72, 32, output.originalHash.begin());
  std::copy_n(bytes.begin() + 104, 32, output.sourcePathHash.begin());
  output.originalLength = length;
  return true;
}
}  // namespace companion
