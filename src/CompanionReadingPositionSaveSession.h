#pragma once

#include "HalInventoryFileHash.h"
#include "HalJournalCausalAuditSession.h"
#include "HalTintaWriterSession.h"

namespace companion {
// Checked off-stack owner. Caller freezes verified book content and excludes
// journal/index writers until completion; success permits derived progress save.
class NativeReadingPositionSaveSession final {
 public:
  TintaJournalResult persistFile(const char* path, const ReadingAnchor& anchor, std::span<uint8_t> scratch,
                                 IdentityStorage& identities) {
    if (used) return TintaJournalResult::Unavailable;
    if (!path || !Storage.openFileForRead("COMPANION", path, book)) {
      used = true;
      return failure("book open");
    }
    TintaJournalResult result;
    if (!hashInventoryFile(book, scratch, length, edition) || !length) {
      used = true;
      result = failure("book hash");
    } else {
      result = persist(anchor, edition, identities);
    }
    if (!book.close()) return failure("book close");
    return result;
  }
  TintaJournalResult persist(const ReadingAnchor& anchor, const Digest& verifiedEdition, IdentityStorage& identities,
                             bool resolveConflict = false) {
    if (used) return TintaJournalResult::Unavailable;
    used = true;
    if (!tinta_body_detail::nonzero(verifiedEdition)) return TintaJournalResult::Invalid;
    body[0] = 1;
    body[1] = static_cast<uint8_t>(EventKind::ReadingPosition);
    body[2] = anchor.spine & 0xff;
    body[3] = anchor.spine >> 8;
    for (unsigned at = 0; at < 4; ++at) body[at + 4] = anchor.visibleTextOffset >> (8 * at);
    auto audit = makeUniqueNoThrow<HalJournalCausalAuditSession>();
    if (!audit || !audit->run()) return failure("authority audit");
    ReadingAnchor current;
    const auto resolved = audit->resolveReadingPosition(verifiedEdition, current);
    if (resolved == TintaJournalResult::Ok && current == anchor) return TintaJournalResult::Ok;
    if (resolved == TintaJournalResult::Conflict && !resolveConflict) return resolved;
    if (resolved != TintaJournalResult::Ok && resolved != TintaJournalResult::Conflict &&
        resolved != TintaJournalResult::Unavailable)
      return failure("current position resolution");
    if (!audit->run()) return failure("authority refresh");
    if (writer.start(identities) != TintaJournalResult::Ok) return failure("identity/writer start");
    auto* heads = audit->beginPreferenceKnowledge(writer.authority().count());
    if (!heads) return failure("knowledge snapshot");
    const auto result =
        writer.mutations().recordReadingPosition(body, verifiedEdition, *heads, 0, ClockQuality::Unknown);
    recovery = !writer.mutations().available();
    const bool headsClosed = audit->endPreferenceKnowledge();
    const bool writerClosed = writer.close();
    if (!headsClosed || !writerClosed) return failure("knowledge/writer close");
    return result;
  }
  bool requiresRecovery() const { return recovery; }

 private:
  TintaJournalResult failure(const char* stage) {
    recovery = true;
    LOG_ERR("COMPANION", "Reading position save failed: %s", stage);
    return TintaJournalResult::IoError;
  }
  HalTintaWriterSession writer;
  HalFile book;
  Digest edition{};
  uint64_t length = 0;
  std::array<uint8_t, 8> body{};
  bool used = false, recovery = false;
};
}  // namespace companion
