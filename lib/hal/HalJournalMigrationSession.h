#pragma once

#include "HalJournalMigrationPublicationStorage.h"

namespace companion {
// Explicit migration request; caller excludes journal writers for the entire call.
class HalJournalMigrationSession {
 public:
  bool run() {
    auto publication = makeUniqueNoThrow<HalJournalMigrationPublicationStorage>();
    if (!publication) return failure("OOM: publication workspace");
    const auto recovered = recoverJournalMigration(*publication);
    if (recovered != JournalMigrationPublicationResult::NoPending &&
        recovered != JournalMigrationPublicationResult::Complete)
      return failure("pending recovery");
    if (publication->directory(JournalMigrationDirectory::Active) != JournalMigrationPresence::Present)
      return failure("active journal missing");
    auto source = makeUniqueNoThrow<HalJournalCausalAuditSession>();
    if (!source) return failure("OOM: source audit workspace");
    JournalMigrationIntent intent;
    if (!source->run(&intent.frontier)) return failure("source audit");
    if (source->recordSize() == TintaJournal::EXTENDED_RECORD_SIZE) return true;
    if (publication->directory(JournalMigrationDirectory::Backup) != JournalMigrationPresence::Missing)
      return failure("existing backup");
    intent.count = source->recordCount();
    if (candidate.open() != TintaJournalResult::Ok || !source->copyTo(candidate)) return failure("candidate copy");
    if (!candidateStorage.close()) return failure("candidate close");
    source.reset();
    if (!publication->authorize(intent)) return failure("authorization");
    if (recoverJournalMigration(*publication) != JournalMigrationPublicationResult::Complete)
      return failure("publication");
    return true;
  }

 private:
  static bool failure(const char* stage) {
    LOG_ERR("COMPANION", "Journal migration failed: %s", stage);
    return false;
  }
  // Allocate this session with makeUniqueNoThrow: fixed scratch/handles exceed the task stack.
  std::array<uint8_t, TintaJournal::EXTENDED_RECORD_SIZE> scratch{};
  HalTintaJournalStorage candidateStorage{TintaJournalLocation::MigrationCandidate};
  TintaJournal candidate{candidateStorage, scratch};
};
}  // namespace companion
