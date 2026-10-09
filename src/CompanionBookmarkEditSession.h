#pragma once

#include <string_view>

#include "BookmarkEntry.h"
#include "CompanionBookmarkSaveSession.h"

namespace companion {
// Checked off-stack one-shot owner. Caller freezes edition/card and excludes
// writers. Only Ok permits derived JSON/UI publication; no string copies.
class NativeBookmarkEditSession final {
 public:
  TintaJournalResult create(BookmarkEntry& entry, const Digest& edition, uint32_t spineCount,
                            IdentityStorage& identities) {
    if (!configure(entry, entry.name, edition, spineCount) || BookmarkIdentity::valid(entry.identity))
      return TintaJournalResult::Invalid;
    if (!identities.randomIdentity(body.identity) || !tinta_body_detail::nonzero(body.identity)) {
      LOG_ERR("COMPANION", "Bookmark creation entropy unavailable");
      return TintaJournalResult::IoError;
    }
    const auto result = save.persistNew(body, edition, identities);
    if (result == TintaJournalResult::Ok) entry.identity = body.identity;
    return result;
  }
  TintaJournalResult rename(const BookmarkEntry& entry, std::string_view name, const Digest& edition,
                            uint32_t spineCount, IdentityStorage& identities) {
    if (!configure(entry, name, edition, spineCount) || !BookmarkIdentity::valid(entry.identity))
      return TintaJournalResult::Invalid;
    body.identity = entry.identity;
    return save.persist(body, edition, identities);
  }
  TintaJournalResult erase(const BookmarkEntry& entry, const Digest& edition, IdentityStorage& identities) {
    if (used || !BookmarkIdentity::valid(entry.identity) || !tinta_body_detail::nonzero(edition))
      return TintaJournalResult::Invalid;
    used = true;
    body.identity = entry.identity;
    body.deleted = true;
    return save.persist(body, edition, identities);
  }
  bool requiresRecovery() const { return save.requiresRecovery(); }

 private:
  bool configure(const BookmarkEntry& entry, std::string_view name, const Digest& edition, uint32_t spineCount) {
    if (used) return false;
    used = true;
    body.name = {reinterpret_cast<const uint8_t*>(name.data()), name.size()};
    body.summary = {reinterpret_cast<const uint8_t*>(entry.summary.data()), entry.summary.size()};
    body.anchor = {entry.computedSpineIndex, entry.visibleTextOffset};
    return tinta_body_detail::nonzero(edition) && spineCount && spineCount <= 65536 && entry.hasVisibleTextOffset &&
           entry.computedSpineIndex < spineCount && name.size() <= BookmarkEntry::MAX_NAME_LENGTH &&
           entry.summary.size() <= 512 && reading_body_detail::text(body.name) &&
           reading_body_detail::text(body.summary);
  }
  NativeBookmarkSaveSession save;
  BookmarkBodyView body{};
  bool used = false;
};
}  // namespace companion
