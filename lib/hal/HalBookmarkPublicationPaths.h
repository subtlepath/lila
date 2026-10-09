#pragma once

#include "CompanionBookmarkCachePath.h"
#include "HalBookmarkJsonStage.h"
#include "HalBookmarkPublicationStorage.h"

namespace companion {
// Checked off-stack owner; initialize once before lending paths to storage.
// Runtime must independently bind legacy active paths to the content edition.
class HalBookmarkPublicationPaths final {
 public:
  static constexpr char ACTIVE_PARENT[] = "/.crosspoint/bookmarks";
  static constexpr char INTENT[] = "/.crosspoint/companion/bookmark-intent";
  static constexpr char INTENT_NEXT[] = "/.crosspoint/companion/bookmark-intent-next";
  bool initialize(std::string_view activePath, const BookmarkPublicationClaim& claim) {
    const auto parent = bookmarkCacheParent(activePath, claim.edition);
    if (ready || !validBookmarkPublicationClaim(claim) || !hal_filename::valid(activePath) || !parent) return false;
    activeParent = parent;
    std::copy(activePath.begin(), activePath.end(), active.begin());
    active[activePath.size()] = 0;
    transaction = claim.transaction;
    edition = claim.edition;
    format(backup, "/.crosspoint/companion/bookmark-backup-");
    format(receipt, "/.crosspoint/companion/bookmark-receipt-");
    format(receiptNext, "/.crosspoint/companion/bookmark-receipt-next-");
    ready = true;
    return true;
  }
  bool matches(const BookmarkPublicationClaim& claim) const {
    return ready && validBookmarkPublicationClaim(claim) && transaction == claim.transaction &&
           edition == claim.edition;
  }
  BookmarkPublicationPaths paths() const {
    if (!ready) return {};
    return {active.data(),  HalBookmarkJsonStage::PATH, backup.data(), INTENT, INTENT_NEXT,
            receipt.data(), receiptNext.data(),         activeParent};
  }
  HalBookmarkPublicationPaths() = default;
  HalBookmarkPublicationPaths(const HalBookmarkPublicationPaths&) = delete;
  HalBookmarkPublicationPaths& operator=(const HalBookmarkPublicationPaths&) = delete;

 private:
  void format(std::array<char, 96>& output, std::string_view prefix) {
    static constexpr char DIGITS[] = "0123456789abcdef";
    std::copy(prefix.begin(), prefix.end(), output.begin());
    size_t at = prefix.size();
    for (const auto byte : transaction) {
      output[at++] = DIGITS[byte >> 4];
      output[at++] = DIGITS[byte & 15];
    }
    output[at] = 0;
  }
  std::array<char, INVENTORY_PATH_LIMIT + 1> active{};
  std::array<char, 96> backup{}, receipt{}, receiptNext{};
  Identity transaction{};
  Digest edition{};
  const char* activeParent = nullptr;
  bool ready = false;
};
}  // namespace companion
