#pragma once

#include "HalJournalMergeCandidateSession.h"
#include "HalJournalMergeStartupRecovery.h"

namespace companion {
// Retain off-stack, before loading reader stores or enabling journal writers.
class HalJournalStartupRecovery final {
 public:
  bool run(IdentityStorage& identities) {
    const auto presence = aborting.load(pending);
    if (presence == JournalMigrationPresence::IoError) return failure("abort intent read");
    if (presence == JournalMigrationPresence::Present) {
      if (provisionIdentity(identities, identity) != IdentityResult::Ok ||
          !recoverExistingJournalMergeAbort(identity.storageGeneration))
        return failure("abort recovery");
      return publication.run(identity.storageGeneration);
    }
    return publication.run(identities);
  }

 private:
  static bool failure(const char* reason) {
    LOG_ERR("COMPANION", "Journal startup recovery failed: %s", reason);
    return false;
  }
  HalJournalMergeRecordStore aborting{JournalMergeRecord::Aborting};
  HalJournalMergeStartupRecovery publication;
  JournalMergeIntent pending;
  IdentityState identity;
};
}  // namespace companion
