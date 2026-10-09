#pragma once

#include "CompanionBookmarkPublicationRecord.h"

namespace companion {
// Owns only the three private preparation spools, never the active bookmark file.
struct BookmarkPreparationClaim {
  Identity transaction{}, storageGeneration{};
  Digest edition{}, destinationPathHash{};
  bool operator==(const BookmarkPreparationClaim&) const = default;
};
inline constexpr size_t BOOKMARK_PREPARATION_CLAIM_SIZE = 108;
inline bool validBookmarkPreparationClaim(const BookmarkPreparationClaim& claim) {
  return tinta_body_detail::nonzero(claim.transaction) && tinta_body_detail::nonzero(claim.storageGeneration) &&
         tinta_body_detail::nonzero(claim.edition) && tinta_body_detail::nonzero(claim.destinationPathHash);
}
inline bool encodeBookmarkPreparationClaim(const BookmarkPreparationClaim& claim, std::span<uint8_t> bytes) {
  if (bytes.size() != BOOKMARK_PREPARATION_CLAIM_SIZE || !validBookmarkPreparationClaim(claim)) return false;
  std::fill(bytes.begin(), bytes.end(), 0);
  bytes[0] = 'B';
  bytes[1] = 'M';
  bytes[2] = 'S';
  bytes[3] = 'C';
  bytes[4] = 1;
  std::copy(claim.transaction.begin(), claim.transaction.end(), bytes.begin() + 8);
  std::copy(claim.storageGeneration.begin(), claim.storageGeneration.end(), bytes.begin() + 24);
  std::copy(claim.edition.begin(), claim.edition.end(), bytes.begin() + 40);
  std::copy(claim.destinationPathHash.begin(), claim.destinationPathHash.end(), bytes.begin() + 72);
  bookmark_record_detail::put(bytes.last(4), bookmark_record_detail::checksum(bytes.first(104)));
  return true;
}
inline bool decodeBookmarkPreparationClaim(std::span<const uint8_t> bytes, BookmarkPreparationClaim& output) {
  if (bytes.size() != BOOKMARK_PREPARATION_CLAIM_SIZE || bytes[0] != 'B' || bytes[1] != 'M' || bytes[2] != 'S' ||
      bytes[3] != 'C' || bytes[4] != 1 || bytes[5] || bytes[6] || bytes[7] ||
      bookmark_record_detail::get(bytes.last(4)) != bookmark_record_detail::checksum(bytes.first(104)) ||
      !tinta_body_detail::nonzero(bytes.subspan(8, 16)) || !tinta_body_detail::nonzero(bytes.subspan(24, 16)) ||
      !tinta_body_detail::nonzero(bytes.subspan(40, 32)) || !tinta_body_detail::nonzero(bytes.subspan(72, 32)))
    return false;
  std::copy_n(bytes.begin() + 8, 16, output.transaction.begin());
  std::copy_n(bytes.begin() + 24, 16, output.storageGeneration.begin());
  std::copy_n(bytes.begin() + 40, 32, output.edition.begin());
  std::copy_n(bytes.begin() + 72, 32, output.destinationPathHash.begin());
  return true;
}
}  // namespace companion
