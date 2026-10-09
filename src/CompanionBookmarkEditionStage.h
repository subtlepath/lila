#pragma once

#include "CompanionBookmarkIdentityEnumeration.h"
#include "CompanionBookmarkReplay.h"
#include "HalBookmarkBodyStage.h"

namespace companion {
// Checked off-stack owner. Caller verifies book content and excludes all writers
// through final publication. Reading stages JSON only; End validates the spool.
class NativeBookmarkEditionStage final {
 public:
  explicit NativeBookmarkEditionStage(std::span<uint8_t> scratch, InventoryHashProgress guard = nullptr,
                                      void* context = nullptr)
      : bodies(scratch, guard, context), scratch(scratch), guard(guard), context(context) {}
  TintaJournalResult prepare(const Digest& edition, uint32_t spineCount, bool allowEmpty = false) {
    if (used) return TintaJournalResult::Unavailable;
    used = true;
    if (!tinta_body_detail::nonzero(edition) || !spineCount || spineCount > 0x10000) return TintaJournalResult::Invalid;
    auto ids = makeUniqueNoThrow<NativeBookmarkIdentityEnumeration>(scratch, guard, context);
    if (!ids) return failure("OOM: ID enumeration");
    const auto enumerated = ids->prepare(edition);
    if (enumerated != TintaJournalResult::Ok) return abort(enumerated, *ids);
    frontier = ids->authorityFrontier();
    count = ids->recordCount();
    stride = ids->recordSize();
    if (!ids->size()) {
      if (!allowEmpty) return abort(TintaJournalResult::Unavailable, *ids);
      Identity identity{};
      if (ids->next(identity) != BookmarkCursorResult::End) return abort(failure("empty ID verification"), *ids);
      if (!ids->cleanup()) return abort(failure("empty ID cleanup"), *ids);
      ids.reset();
      auto audit = makeUniqueNoThrow<HalJournalCausalAuditSession>();
      if (!audit || !audit->run(&rechecked)) return failure("empty authority recheck");
      if (frontier != rechecked || count != audit->recordCount() || stride != audit->recordSize())
        return TintaJournalResult::Conflict;
      ready = empty = true;
      return TintaJournalResult::Ok;
    }
    if (!bodies.begin(ids->size())) return abort(failure("body stage begin"), *ids);
    auto replay = makeUniqueNoThrow<NativeBookmarkReplay>();
    if (!replay) return abort(failure("OOM: bookmark replay"), *ids);
    Identity identity{};
    for (;;) {
      const auto next = ids->next(identity);
      if (next == BookmarkCursorResult::End) break;
      if (next != BookmarkCursorResult::Found) return abort(failure("ID read"), *ids);
      const auto resolved = replay->run(edition, identity, spineCount, append, &bodies);
      if (resolved != TintaJournalResult::Ok) {
        if (resolved == TintaJournalResult::Conflict && replay->hasConcurrentVersions()) conflict = identity;
        const auto result = abort(resolved, *ids);
        if (result != TintaJournalResult::Conflict) conflict = {};
        return result;
      }
      vTaskDelay(1);
    }
    replay.reset();
    if (!ids->cleanup()) return abort(failure("ID cleanup"), *ids);
    ids.reset();
    if (!bodies.seal()) return cleanupFailure("body stage seal");
    auto audit = makeUniqueNoThrow<HalJournalCausalAuditSession>();
    if (!audit || !audit->run(&rechecked)) return cleanupFailure("authority recheck");
    if (frontier != rechecked || count != audit->recordCount() || stride != audit->recordSize()) {
      if (!bodies.cleanup()) return failure("changed authority cleanup");
      return TintaJournalResult::Conflict;
    }
    audit.reset();
    ready = true;
    return TintaJournalResult::Ok;
  }
  BookmarkCursorResult next(std::span<const uint8_t>& output) {
    if (!ready) return BookmarkCursorResult::Error;
    if (empty) {
      if (guard && !guard(context)) {
        ready = false;
        failure("empty read authority");
        return BookmarkCursorResult::Error;
      }
      return BookmarkCursorResult::End;
    }
    const auto result = bodies.next(output);
    if (result == BookmarkCursorResult::Error) ready = false;
    return result;
  }
  bool cleanup() {
    ready = false;
    return bodies.cleanup();
  }
  uint32_t size() const { return ready ? bodies.size() : 0; }
  const Digest& authorityFrontier() const { return frontier; }
  uint32_t recordCount() const { return count; }
  uint16_t recordSize() const { return stride; }
  const Identity& conflictIdentity() const { return conflict; }

 private:
  static bool append(void* context, std::span<const uint8_t> body) {
    return static_cast<HalBookmarkBodyStage*>(context)->append(body);
  }
  static TintaJournalResult failure(const char* operation) {
    LOG_ERR("COMPANION", "Bookmark edition staging failed: %s", operation);
    return TintaJournalResult::IoError;
  }
  TintaJournalResult abort(TintaJournalResult result, NativeBookmarkIdentityEnumeration& ids) {
    const bool idsClean = ids.cleanup();
    const bool bodiesClean = bodies.cleanup();
    return idsClean && bodiesClean ? result : failure("stage cleanup");
  }
  TintaJournalResult cleanupFailure(const char* operation) {
    bodies.cleanup();
    return failure(operation);
  }
  HalBookmarkBodyStage bodies;
  std::span<uint8_t> scratch;
  InventoryHashProgress guard;
  void* context;
  Digest frontier{}, rechecked{};
  Identity conflict{};
  uint32_t count = 0;
  uint16_t stride = 0;
  bool used = false, ready = false, empty = false;
};
}  // namespace companion
