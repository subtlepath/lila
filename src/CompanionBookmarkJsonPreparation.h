#pragma once

#include "CompanionBookmarkEditionStage.h"
#include "HalBookmarkJsonStage.h"

namespace companion {
// Checked off-stack owner. Caller verifies content, imports legacy bookmarks and
// excludes all writers through publication. Scratch outlives this owner.
class NativeBookmarkJsonPreparation final {
 public:
  explicit NativeBookmarkJsonPreparation(std::span<uint8_t> scratch, InventoryHashProgress guard = nullptr,
                                         void* context = nullptr)
      : edition(scratch, guard, context), json(scratch, guard, context) {}
  TintaJournalResult prepare(const Digest& verifiedEdition, uint32_t spineCount, bool allowEmpty = false) {
    if (used) return TintaJournalResult::Unavailable;
    used = true;
    const auto prepared = edition.prepare(verifiedEdition, spineCount, allowEmpty);
    if (prepared != TintaJournalResult::Ok) return discard(prepared);
    editionHash = verifiedEdition;
    frontier = edition.authorityFrontier();
    count = edition.recordCount();
    stride = edition.recordSize();
    if (!json.begin(edition.size())) return discard(failure("JSON stage begin"));
    std::span<const uint8_t> body;
    for (;;) {
      const auto next = edition.next(body);
      if (next == BookmarkCursorResult::End) break;
      if (next != BookmarkCursorResult::Found || !json.append(body)) return discard(failure("body/JSON stream"));
      vTaskDelay(1);
    }
    if (!edition.cleanup()) return discard(failure("body cleanup"));
    if (!json.finish()) return discard(failure("JSON seal"));
    auto audit = makeUniqueNoThrow<HalJournalCausalAuditSession>();
    if (!audit || !audit->run(&rechecked)) return discard(failure("authority recheck"));
    if (frontier != rechecked || count != audit->recordCount() || stride != audit->recordSize())
      return discard(TintaJournalResult::Conflict);
    audit.reset();
    ready = true;
    return TintaJournalResult::Ok;
  }
  bool isPrepared() const { return ready && json.isSealed(); }
  uint64_t bytesWritten() const { return json.bytesWritten(); }
  const Digest& contentHash() const { return json.contentHash(); }
  const Digest& contentEdition() const { return editionHash; }
  const Digest& authorityFrontier() const { return frontier; }
  uint32_t recordCount() const { return count; }
  uint16_t recordSize() const { return stride; }
  const Identity& conflictIdentity() const { return edition.conflictIdentity(); }
  bool retainForPublication(HalBookmarkPublicationRecord& records, const char* intentPath,
                            const BookmarkPublicationClaim& claim) {
    if (!isPrepared() || claim.edition != editionHash || claim.frontier != frontier || claim.recordCount != count ||
        claim.recordSize != stride || !json.retainForPublication(records, intentPath, claim))
      return false;
    ready = false;
    return true;
  }
  bool persistPublicationIntent(BookmarkPublicationStorage& storage, const BookmarkPublicationClaim& claim) {
    if (!isPrepared() || claim.edition != editionHash || claim.frontier != frontier || claim.recordCount != count ||
        claim.recordSize != stride)
      return false;
    ready = false;
    return json.persistPublicationIntent(storage, claim);
  }
  bool cleanup() {
    ready = false;
    const bool editionClean = edition.cleanup();
    const bool jsonClean = json.cleanup();
    return editionClean && jsonClean;
  }

 private:
  static TintaJournalResult failure(const char* operation) {
    LOG_ERR("COMPANION", "Bookmark JSON preparation failed: %s", operation);
    return TintaJournalResult::IoError;
  }
  TintaJournalResult discard(TintaJournalResult result) { return cleanup() ? result : failure("candidate cleanup"); }
  NativeBookmarkEditionStage edition;
  HalBookmarkJsonStage json;
  Digest editionHash{}, frontier{}, rechecked{};
  uint32_t count = 0;
  uint16_t stride = 0;
  bool used = false, ready = false;
};
}  // namespace companion
