#pragma once

#include "HalJournalCausalAuditSession.h"
#include "HalTintaWriterSession.h"

namespace companion {
// Checked off-stack owner. Caller freezes verified book content and excludes
// journal/index writers; only success permits publishing derived bookmark state.
class NativeBookmarkSaveSession final {
 public:
  TintaJournalResult persist(const BookmarkBodyView& bookmark, const Digest& verifiedEdition,
                             IdentityStorage& identities, bool resolveConflict = false) {
    return persistImpl(bookmark, verifiedEdition, identities, resolveConflict, false);
  }
  TintaJournalResult persistNew(const BookmarkBodyView& bookmark, const Digest& verifiedEdition,
                                IdentityStorage& identities) {
    return persistImpl(bookmark, verifiedEdition, identities, false, true);
  }
  TintaJournalResult resolve(const BookmarkBodyView& bookmark, const Digest& verifiedEdition,
                             IdentityStorage& identities, const Digest& shownFrontier) {
    return persistImpl(bookmark, verifiedEdition, identities, true, false, &shownFrontier);
  }
  bool requiresRecovery() const { return recovery; }

 private:
  TintaJournalResult persistImpl(const BookmarkBodyView& bookmark, const Digest& verifiedEdition,
                                 IdentityStorage& identities, bool resolveConflict, bool requireAbsent,
                                 const Digest* shownFrontier = nullptr) {
    if (used) return TintaJournalResult::Unavailable;
    used = true;
    if (!tinta_body_detail::nonzero(verifiedEdition)) return TintaJournalResult::Invalid;
    const auto length = encodeBookmarkBody(bookmark, body);
    if (!length) return TintaJournalResult::Invalid;
    auto audit = makeUniqueNoThrow<HalJournalCausalAuditSession>();
    if (!audit || !audit->run(shownFrontier ? &frontier : nullptr)) return failure("authority audit");
    if (shownFrontier && frontier != *shownFrontier) return TintaJournalResult::Conflict;
    size_t currentLength = 0;
    const auto resolved = audit->resolveBookmark(verifiedEdition, bookmark.identity, current, currentLength);
    if (requireAbsent && resolved != TintaJournalResult::Unavailable) {
      if (resolved == TintaJournalResult::Ok || resolved == TintaJournalResult::Conflict)
        return TintaJournalResult::Conflict;
      return failure("new bookmark identity audit");
    }
    if (resolved == TintaJournalResult::Ok && currentLength == length &&
        std::equal(body.begin(), body.begin() + length, current.begin()))
      return TintaJournalResult::Ok;
    if (resolved == TintaJournalResult::Conflict && !resolveConflict) return resolved;
    if (resolved != TintaJournalResult::Ok && resolved != TintaJournalResult::Conflict &&
        resolved != TintaJournalResult::Unavailable)
      return failure("current bookmark resolution");
    if (!audit->run(shownFrontier ? &frontier : nullptr)) return failure("authority refresh");
    if (shownFrontier && frontier != *shownFrontier) return TintaJournalResult::Conflict;
    if (writer.start(identities) != TintaJournalResult::Ok) return failure("identity/writer start");
    auto* heads = audit->beginPreferenceKnowledge(writer.authority().count());
    if (!heads) return failure("knowledge snapshot");
    const auto result = writer.mutations().recordBookmark(std::span(body).first(length), verifiedEdition, *heads, 0,
                                                          ClockQuality::Unknown);
    recovery = !writer.mutations().available();
    const bool headsClosed = audit->endPreferenceKnowledge();
    const bool writerClosed = writer.close();
    if (!headsClosed || !writerClosed) return failure("knowledge/writer close");
    return result;
  }
  TintaJournalResult failure(const char* stage) {
    recovery = true;
    LOG_ERR("COMPANION", "Bookmark save failed: %s", stage);
    return TintaJournalResult::IoError;
  }
  HalTintaWriterSession writer;
  Digest frontier{};
  std::array<uint8_t, MAX_BOOKMARK_BODY_SIZE> body{}, current{};
  bool used = false, recovery = false;
};
}  // namespace companion
