#pragma once

#include "HalDictionaryExtractionJournalStorage.h"
#include "HalInventoryFileHash.h"
#include "HalZipNameBytesStorage.h"
#include "HalZipRangeStorage.h"

namespace companion {
// Retained in a checked session owner, never a task-local object. No destructor
// deletes persistent proofs; restart recovery must still be able to consume them.
class HalDictionaryZipAudit final {
 public:
  HalDictionaryZipAudit(const TransferState& state, const ContentManifest& manifest, const Identity& generation,
                        std::string_view base, std::span<uint8_t> scratch, InventoryHashProgress progress = nullptr,
                        void* context = nullptr)
      : journal(storage, state, manifest, generation, base, scratch),
        state(state),
        progress(progress),
        context(context),
        lookup(progress, context) {}
  bool begin() {
    if (state.phase != TransferPhase::Receiving && state.phase != TransferPhase::Verified) return fail("begin phase");
    const auto result = journal.recover();
    if (result == DictionaryJournalResult::Missing) {
      if (!indexesAbsent()) return fail("unowned indexes");
    } else if (result != DictionaryJournalResult::Ok || !discardIndexes())
      return fail("proof recovery");
    return journal.publish() == DictionaryJournalResult::Ok || fail("proof publication");
  }
  bool finish() {
    if (!journal.current() || !indexesAbsent()) return fail("indexes remain");
    for (const auto path : DICTIONARY_ZIP_AUDIT_JOURNALS)
      if (!remove(path)) return false;
    return authorized() || fail("changed parent");
  }
  bool abort() {
    if (state.phase != TransferPhase::Aborted) return fail("abort phase");
    // Incomplete uploads have no audit proof; do not create one during Abort.
    bool missing = true;
    for (const auto path : DICTIONARY_ZIP_AUDIT_JOURNALS) {
      const auto presence = lookup.inspect(path);
      if (presence == CompanionFilePresence::Error) return fail("proof lookup");
      missing &= presence == CompanionFilePresence::Missing;
    }
    if (missing) return indexesAbsent() || fail("unowned indexes");
    if (journal.recover() != DictionaryJournalResult::Ok || !discardIndexes()) return fail("abort proof");
    return finish();
  }

 private:
  HalDictionaryExtractionJournalStorage storage{HalDictionaryExtractionJournalStorage::Purpose::ZipAudit};
  DictionaryZipAuditJournal journal;
  const TransferState& state;
  InventoryHashProgress progress;
  void* context;
  HalCompanionFileLookup lookup;
  HalFile file;
  static constexpr const char* INDEXES[] = {HalZipRangeStorage::PATH, HalZipRangeStorage::NAME_INDEX_PATH,
                                            HalZipNameBytesStorage::PATH};
  static constexpr uint64_t LIMITS[] = {20000ULL * 16, 20000ULL * 16, 20000ULL * 3072};
  bool authorized() const { return journal.current() && (!progress || progress(context)) && journal.current(); }
  bool indexesAbsent() {
    for (const auto path : INDEXES)
      if (lookup.inspect(path) != CompanionFilePresence::Missing) return false;
    return true;
  }
  bool discardIndexes() {
    // Check every artifact before mutation, including checked close/readback.
    for (unsigned at = 0; at < 3; ++at) {
      if (!authorized()) return fail("changed parent");
      const auto presence = lookup.inspect(INDEXES[at]);
      if (presence == CompanionFilePresence::Error) return fail("index lookup");
      if (presence == CompanionFilePresence::Missing) continue;
      if (!Storage.openFileForReadReusing("COMPANION", INDEXES[at], file)) return fail("index open");
      const bool regular = !file.isDirectory() && file.fileSize64() <= LIMITS[at];
      const bool closed = file.close();
      if (!regular || !closed || !authorized()) return fail("index type or close");
    }
    for (const auto path : INDEXES)
      if (!remove(path)) return false;
    return (authorized() && indexesAbsent()) || fail("index absence or changed parent");
  }
  bool remove(const char* path) {
    if (!authorized()) return fail("changed parent");
    const auto presence = lookup.inspect(path);
    if (presence == CompanionFilePresence::Missing) return authorized() || fail("changed parent");
    if (presence != CompanionFilePresence::Present || !authorized() || !Storage.remove(path) || !authorized() ||
        lookup.inspect(path) != CompanionFilePresence::Missing || !authorized())
      return fail("removal or absence");
    return true;
  }
  static bool fail(const char* operation) {
    LOG_ERR("COMPANION", "Dictionary ZIP audit %s failed", operation);
    return false;
  }
};
}  // namespace companion
