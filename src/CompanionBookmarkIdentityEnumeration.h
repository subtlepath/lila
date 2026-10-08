#pragma once

#include "HalBookmarkIdentityStage.h"
#include "HalJournalCausalAuditSession.h"

namespace companion {
// Checked off-stack owner. Scratch and frozen journal/content outlive enumeration.
// Consumers stage results only until next() returns End, then recheck authority.
class NativeBookmarkIdentityEnumeration final {
 public:
  explicit NativeBookmarkIdentityEnumeration(std::span<uint8_t> scratch) : stage(scratch) {}
  TintaJournalResult prepare(const Digest& edition) {
    if (used) return TintaJournalResult::Unavailable;
    used = true;
    if (!tinta_body_detail::nonzero(edition)) return TintaJournalResult::Invalid;
    auto audit = makeUniqueNoThrow<HalJournalCausalAuditSession>();
    if (!audit || !audit->run(&frontier)) return failure("authority audit");
    journalCount = audit->recordCount();
    stride = audit->recordSize();
    if (!stage.begin(journalCount)) return failure("ID stage begin");
    const auto result = audit->bookmarkIdentities(edition, &stage, append);
    audit.reset();
    if (result != TintaJournalResult::Ok) return result;
    if (!stage.seal()) return failure("ID stage seal");
    return TintaJournalResult::Ok;
  }
  BookmarkCursorResult next(Identity& output) { return stage.next(output); }
  bool cleanup() { return stage.cleanup(); }
  uint32_t size() const { return stage.size(); }
  uint32_t recordCount() const { return journalCount; }
  uint16_t recordSize() const { return stride; }
  const Digest& authorityFrontier() const { return frontier; }

 private:
  static bool append(void* context, const Identity& identity) {
    return static_cast<HalBookmarkIdentityStage*>(context)->append(identity);
  }
  static TintaJournalResult failure(const char* operation) {
    LOG_ERR("COMPANION", "Bookmark enumeration failed: %s", operation);
    return TintaJournalResult::IoError;
  }
  HalBookmarkIdentityStage stage;
  Digest frontier{};
  uint32_t journalCount = 0;
  uint16_t stride = 0;
  bool used = false;
};
}  // namespace companion
