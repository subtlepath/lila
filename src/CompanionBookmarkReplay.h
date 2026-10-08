#pragma once

#include "HalJournalCausalAuditSession.h"

namespace companion {
// Retain off-stack. Caller verifies book content and excludes journal/content
// writers through publication. Publisher receives a borrowed canonical body.
class NativeBookmarkReplay final {
 public:
  using Publisher = bool (*)(void*, std::span<const uint8_t>);
  TintaJournalResult run(const Digest& edition, const Identity& bookmark, uint32_t spineCount, Publisher publish,
                         void* context) {
    if (!publish || !spineCount || spineCount > 0x10000 || !tinta_body_detail::nonzero(edition) ||
        !tinta_body_detail::nonzero(bookmark))
      return TintaJournalResult::Invalid;
    auto audit = makeUniqueNoThrow<HalJournalCausalAuditSession>();
    if (!audit || !audit->run(&frontier)) return failure("authority audit");
    const auto count = audit->recordCount();
    const auto recordSize = audit->recordSize();
    size_t length = 0;
    const auto resolved = audit->resolveBookmark(edition, bookmark, body, length);
    audit.reset();
    if (resolved != TintaJournalResult::Ok) return resolved;
    BookmarkBodyView decoded;
    if (!decodeBookmarkBody(std::span(body).first(length), decoded)) return TintaJournalResult::Corrupt;
    if (!decoded.deleted && decoded.anchor.spine >= spineCount) return TintaJournalResult::Invalid;
    audit = makeUniqueNoThrow<HalJournalCausalAuditSession>();
    if (!audit || !audit->run(&rechecked)) return failure("authority recheck");
    if (frontier != rechecked || count != audit->recordCount() || recordSize != audit->recordSize())
      return TintaJournalResult::Conflict;
    audit.reset();
    return publish(context, std::span(body).first(length)) ? TintaJournalResult::Ok : failure("bookmark publication");
  }

 private:
  static TintaJournalResult failure(const char* stage) {
    LOG_ERR("COMPANION", "Bookmark replay failed: %s", stage);
    return TintaJournalResult::IoError;
  }
  Digest frontier{}, rechecked{};
  std::array<uint8_t, MAX_BOOKMARK_BODY_SIZE> body{};
};
}  // namespace companion
