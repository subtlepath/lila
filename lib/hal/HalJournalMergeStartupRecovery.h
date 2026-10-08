#pragma once

#include "HalIdentityStorage.h"
#include "HalJournalMergePublicationStorage.h"

namespace companion {
// Caller excludes journal writers. Allocate outside the task stack.
class HalJournalMergeStartupRecovery {
 public:
  bool run(IdentityStorage& identities) {
    if (!inspect()) return false;
    if (!required) return true;
    if (provisionIdentity(identities, identity) != IdentityResult::Ok) return failure("SD identity");
    return finish(identity.storageGeneration);
  }
  bool run(const Identity& generation) {
    if (!inspect()) return false;
    if (!required) return true;
    return finish(generation);
  }

 private:
  bool inspect() {
    required = publishing = false;
    const auto intent = publication.intent(pending);
    if (intent == JournalMigrationPresence::IoError) return failure("intent read");
    if (intent == JournalMigrationPresence::Present) {
      required = publishing = true;
      return true;
    }
    const auto checkpoint = receiving.load(pending);
    if (checkpoint == JournalMigrationPresence::Missing) return true;
    if (checkpoint != JournalMigrationPresence::Present) return failure("receiving read");
    const auto receipt = receipts.load(completed);
    if (receipt == JournalMigrationPresence::IoError) return failure("receipt read");
    required = receipt == JournalMigrationPresence::Present && completed == pending;
    return true;
  }
  bool finish(const Identity& generation) {
    if (publishing) {
      if (recoverJournalMerge(publication, generation) != JournalMigrationPublicationResult::Complete)
        return failure("publication");
    } else if (pending.generation != generation ||
               publication.directory(JournalMergeDirectory::Candidate) != JournalMigrationPresence::Missing ||
               !publication.verify(JournalMergeDirectory::Active, pending.merged)) {
      return failure("completed checkpoint authority");
    }
    return receiving.clear(pending) || failure("receiving checkpoint");
  }
  static bool failure(const char* stage) {
    LOG_ERR("COMPANION", "Journal merge startup recovery failed: %s", stage);
    return false;
  }
  HalJournalMergePublicationStorage publication;
  HalJournalMergeRecordStore receiving{JournalMergeRecord::Receiving};
  HalJournalMergeRecordStore receipts{JournalMergeRecord::Receipt};
  JournalMergeIntent pending, completed;
  IdentityState identity;
  bool required = false, publishing = false;
};

inline bool recoverExistingJournalMerge(HalCompanionFileLookup& lookup) {
  const auto presence = lookup.inspect(HalJournalMergeRecordStore::INTENT);
  if (presence == CompanionFilePresence::Missing) {
    const auto checkpoint = lookup.inspect(HalJournalMergeRecordStore::RECEIVING);
    if (checkpoint == CompanionFilePresence::Missing) return true;
    if (checkpoint == CompanionFilePresence::Error) {
      LOG_ERR("COMPANION", "Cannot inspect journal receiving checkpoint");
      return false;
    }
  }
  if (presence == CompanionFilePresence::Error) {
    LOG_ERR("COMPANION", "Cannot inspect journal merge startup intent");
    return false;
  }
  // Storage handles and publication state exceed the stack budget; absent intents allocate nothing.
  auto recovery = makeUniqueNoThrow<HalJournalMergeStartupRecovery>();
  if (!recovery) {
    LOG_ERR("COMPANION", "OOM: journal merge startup recovery workspace");
    return false;
  }
  HalIdentityStorage identities;
  return recovery->run(identities);
}
}  // namespace companion
