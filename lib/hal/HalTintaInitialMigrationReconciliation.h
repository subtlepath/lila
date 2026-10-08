#pragma once

#include "CompanionIdentity.h"
#include "HalLegacyTintaBackupSession.h"
#include "HalTintaMigrationAdmissions.h"
#include "HalTintaProvenPublication.h"
#include "HalTintaReplayExport.h"
#include "HalTintaReplaySession.h"

namespace companion {
// Checked off-stack owner. Caller verifies installed pack/catalog, requires no
// canonical receipt, and excludes native/journal writers through publication.
class HalTintaInitialMigrationReconciliation final {
 public:
  TintaMigrationAdmissionResult lookup(const Identity& course, const JournalMergeIntent& merge) {
    auto records = makeUniqueNoThrow<HalTintaMigrationAdmissions>(hashing);
    if (!records) {
      failure("OOM: admission records");
      return TintaMigrationAdmissionResult::IoError;
    }
    return records->load(course, merge, admission);
  }
  bool run(const Identity& course, const IdentityState& identity, const Digest& resource,
           const JournalMergeIntent& merge, TintaSubjectCatalog& catalog) {
    studyDay = 0;
    if (merge.generation != identity.storageGeneration) return failure("storage generation");
    if (lookup(course, merge) != TintaMigrationAdmissionResult::Ok || admission.reader != identity.device ||
        admission.resource != resource)
      return failure("migration admission binding");
    {
      auto backup = makeUniqueNoThrow<HalLegacyTintaBackupSession>();
      if (!backup) return failure("OOM: reviewed backup");
      if (!backup->prepareCourse(identity.device, identity.storageGeneration, course, admission.backupTransaction, true,
                                 true) ||
          !backup->recover() || !backup->verifyMigrationBackup(admission, hashing))
        return failure("reviewed native sources");
    }
    {
      auto audit = makeUniqueNoThrow<HalJournalCausalAuditSession>();
      if (!audit) return failure("OOM: merged journal audit");
      Digest frontier{}, previous{};
      if (!audit->run(&frontier, &course, &catalog) || frontier != merge.merged.frontier ||
          audit->recordCount() != merge.merged.count || audit->recordSize() != merge.merged.recordSize ||
          !audit->prefixFrontier(merge.previous.count, previous) || previous != merge.previous.frontier ||
          !audit->replay(this, inspectEvent))
        return failure("merged history/retention");
    }
    auto replay = makeUniqueNoThrow<HalTintaReplaySession>();
    auto output = makeUniqueNoThrow<HalTintaReplayExport>();
    if (!replay || !output) return failure("OOM: replay/export");
    if (!replay->run(course, catalog) || !replay->journalFrontier() ||
        *replay->journalFrontier() != merge.merged.frontier ||
        !output->run(*replay->workingStore(), course, studyDay, scratch) ||
        !output->manifest(identity.storageGeneration, resource, merge.merged.frontier, merge.transaction, 1,
                          manifest) ||
        !replay->workingStore()->close())
      return failure("initial replay/export");
    output.reset();
    replay.reset();
    return publishProvenTintaDerived(course, identity.storageGeneration, resource, catalog, manifest, scratch) ==
               TintaPublicationResult::Ok ||
           failure("initial publication");
  }

 private:
  static bool inspectEvent(void* context, uint32_t, const SyncEvent& event, std::span<const uint8_t> bytes, bool) {
    auto& owner = *static_cast<HalTintaInitialMigrationReconciliation*>(context);
    if (event.kind < EventKind::Review || event.kind > EventKind::ReadingComplete) return true;
    if (!decodeTintaBody(bytes, owner.body)) return false;
    if (owner.body.course != owner.admission.course) return true;
    owner.studyDay = std::max(owner.studyDay, static_cast<uint16_t>(event.studyDay));
    return true;
  }
  static bool failure(const char* reason) {
    LOG_ERR("COMPANION", "Initial Tinta migration reconciliation failed: %s", reason);
    return false;
  }
  HalTintaJournalStorage hashing;
  TintaMigrationAdmission admission;
  TintaBody body;
  uint16_t studyDay = 0;
  std::array<uint8_t, 512> scratch{};
  std::array<uint8_t, TINTA_DERIVED_MANIFEST_SIZE> manifest{};
};
}  // namespace companion
