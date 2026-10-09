#pragma once

#include "CompanionBookmarkMigrationClaim.h"

namespace companion {
// Checked off-stack owner; initialize/bind once before lending any paths.
class HalBookmarkMigrationPaths final {
 public:
  bool initialize(const Digest& edition, const Digest& sourcePathHash) {
    if (initialized || !tinta_body_detail::nonzero(edition) || !tinta_body_detail::nonzero(sourcePathHash))
      return false;
    this->edition = edition;
    this->sourcePathHash = sourcePathHash;
    format(claimPath, "/.crosspoint/companion/bookmark-migration-", edition, sourcePathHash);
    format(claimNext, "/.crosspoint/companion/bookmark-migration-next-", edition, sourcePathHash);
    initialized = true;
    return true;
  }
  bool bind(const BookmarkMigrationClaim& claim) {
    if (!initialized || bound || !validBookmarkMigrationClaim(claim) || claim.edition != edition ||
        claim.sourcePathHash != sourcePathHash)
      return false;
    format(backupPath, "/.crosspoint/companion/bookmark-original-", edition, claim.transaction);
    format(backupNext, "/.crosspoint/companion/bookmark-original-next-", edition, claim.transaction);
    bound = true;
    return true;
  }
  const char* record() const { return initialized ? claimPath.data() : nullptr; }
  const char* recordTemporary() const { return initialized ? claimNext.data() : nullptr; }
  const char* backup() const { return bound ? backupPath.data() : nullptr; }
  const char* backupTemporary() const { return bound ? backupNext.data() : nullptr; }

 private:
  static void format(std::array<char, 192>& output, std::string_view prefix, std::span<const uint8_t> edition,
                     std::span<const uint8_t> transaction = {}) {
    static constexpr char DIGITS[] = "0123456789abcdef";
    std::copy(prefix.begin(), prefix.end(), output.begin());
    size_t at = prefix.size();
    for (const auto byte : edition) {
      output[at++] = DIGITS[byte >> 4];
      output[at++] = DIGITS[byte & 15];
    }
    if (!transaction.empty()) output[at++] = '-';
    for (const auto byte : transaction) {
      output[at++] = DIGITS[byte >> 4];
      output[at++] = DIGITS[byte & 15];
    }
    output[at] = 0;
  }
  std::array<char, 192> claimPath{}, claimNext{}, backupPath{}, backupNext{};
  Digest edition{}, sourcePathHash{};
  bool initialized = false, bound = false;
};
}  // namespace companion
