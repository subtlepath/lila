#pragma once

#include <algorithm>

#include "CompanionBookmarkPublication.h"

namespace companion {
inline constexpr size_t BOOKMARK_PUBLICATION_RECORD_SIZE = 196;
namespace bookmark_record_detail {
inline uint32_t checksum(std::span<const uint8_t> bytes) {
  uint32_t value = UINT32_MAX;
  for (const auto byte : bytes) {
    value ^= byte;
    for (unsigned bit = 0; bit < 8; ++bit) value = (value >> 1) ^ (0xedb88320U & (0U - (value & 1U)));
  }
  return ~value;
}
inline void put(std::span<uint8_t> bytes, uint64_t value) {
  for (auto& byte : bytes) {
    byte = static_cast<uint8_t>(value);
    value >>= 8;
  }
}
inline uint64_t get(std::span<const uint8_t> bytes) {
  uint64_t value = 0;
  for (size_t i = 0; i < bytes.size(); ++i) value |= uint64_t(bytes[i]) << (8 * i);
  return value;
}
}  // namespace bookmark_record_detail
inline bool encodeBookmarkPublicationRecord(const BookmarkPublicationClaim& claim, std::span<uint8_t> bytes) {
  if (bytes.size() != BOOKMARK_PUBLICATION_RECORD_SIZE || !validBookmarkPublicationClaim(claim)) return false;
  std::fill(bytes.begin(), bytes.end(), 0);
  bytes[0] = 'B';
  bytes[1] = 'M';
  bytes[2] = 'P';
  bytes[3] = 'C';
  bytes[4] = 1;
  bytes[5] = claim.hadOriginal;
  std::copy(claim.transaction.begin(), claim.transaction.end(), bytes.begin() + 8);
  std::copy(claim.storageGeneration.begin(), claim.storageGeneration.end(), bytes.begin() + 24);
  std::copy(claim.edition.begin(), claim.edition.end(), bytes.begin() + 40);
  std::copy(claim.frontier.begin(), claim.frontier.end(), bytes.begin() + 72);
  std::copy(claim.candidateHash.begin(), claim.candidateHash.end(), bytes.begin() + 104);
  std::copy(claim.originalHash.begin(), claim.originalHash.end(), bytes.begin() + 136);
  bookmark_record_detail::put(bytes.subspan(168, 8), claim.candidateLength);
  bookmark_record_detail::put(bytes.subspan(176, 8), claim.originalLength);
  bookmark_record_detail::put(bytes.subspan(184, 4), claim.recordCount);
  bookmark_record_detail::put(bytes.subspan(188, 2), claim.recordSize);
  bookmark_record_detail::put(bytes.last(4), bookmark_record_detail::checksum(bytes.first(192)));
  return true;
}
inline bool decodeBookmarkPublicationRecord(std::span<const uint8_t> bytes, BookmarkPublicationClaim& output) {
  if (bytes.size() != BOOKMARK_PUBLICATION_RECORD_SIZE || bytes[0] != 'B' || bytes[1] != 'M' || bytes[2] != 'P' ||
      bytes[3] != 'C' || bytes[4] != 1 || bytes[5] > 1 || bytes[6] || bytes[7] || bytes[190] || bytes[191] ||
      bookmark_record_detail::get(bytes.last(4)) != bookmark_record_detail::checksum(bytes.first(192)))
    return false;
  BookmarkPublicationClaim claim;
  std::copy_n(bytes.begin() + 8, 16, claim.transaction.begin());
  std::copy_n(bytes.begin() + 24, 16, claim.storageGeneration.begin());
  std::copy_n(bytes.begin() + 40, 32, claim.edition.begin());
  std::copy_n(bytes.begin() + 72, 32, claim.frontier.begin());
  std::copy_n(bytes.begin() + 104, 32, claim.candidateHash.begin());
  std::copy_n(bytes.begin() + 136, 32, claim.originalHash.begin());
  claim.candidateLength = bookmark_record_detail::get(bytes.subspan(168, 8));
  claim.originalLength = bookmark_record_detail::get(bytes.subspan(176, 8));
  claim.recordCount = static_cast<uint32_t>(bookmark_record_detail::get(bytes.subspan(184, 4)));
  claim.recordSize = static_cast<uint16_t>(bookmark_record_detail::get(bytes.subspan(188, 2)));
  claim.hadOriginal = bytes[5];
  if (!validBookmarkPublicationClaim(claim)) return false;
  output = claim;
  return true;
}
}  // namespace companion
