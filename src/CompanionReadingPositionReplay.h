#pragma once

#include "HalJournalCausalAuditSession.h"

namespace companion {
// Caller verifies edition/book bounds and excludes journal/content/progress
// writers through publication. Publisher must return only after checked storage.
class NativeReadingPositionReplay final {
 public:
  using Publisher = bool (*)(void*, const ReadingAnchor&);
  TintaJournalResult run(const Digest& edition, uint32_t spineCount, Publisher publish, void* context) {
    if (!publish || !spineCount || spineCount > 0x10000 || !tinta_body_detail::nonzero(edition))
      return TintaJournalResult::Invalid;
    // Audit/index owners exceed the stack; release them before progress publication.
    auto audit = makeUniqueNoThrow<HalJournalCausalAuditSession>();
    if (!audit || !audit->run(&frontier)) return failure("authority audit");
    const auto count = audit->recordCount();
    const auto recordSize = audit->recordSize();
    const auto resolved = audit->resolveReadingPosition(edition, anchor);
    audit.reset();
    if (resolved != TintaJournalResult::Ok) return resolved;
    if (anchor.spine >= spineCount) return TintaJournalResult::Invalid;
    audit = makeUniqueNoThrow<HalJournalCausalAuditSession>();
    if (!audit || !audit->run(&rechecked)) return failure("authority recheck");
    if (frontier != rechecked || count != audit->recordCount() || recordSize != audit->recordSize())
      return TintaJournalResult::Conflict;
    audit.reset();
    return publish(context, anchor) ? TintaJournalResult::Ok : failure("progress publication");
  }

 private:
  static TintaJournalResult failure(const char* stage) {
    LOG_ERR("COMPANION", "Reading position replay failed: %s", stage);
    return TintaJournalResult::IoError;
  }
  Digest frontier{}, rechecked{};
  ReadingAnchor anchor;
};
}  // namespace companion
