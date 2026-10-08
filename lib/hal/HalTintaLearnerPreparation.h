#pragma once

#if LILA_TINTA
#include "HalTintaDerivedStartupRecovery.h"
#include "HalTintaIncrementalRecovery.h"
#include "HalTintaLegacyAdmission.h"
#include "HalTintaNativeDerivedPreparation.h"
#include "HalTintaPreferenceApplication.h"

namespace companion {
// Checked owner; retain its validated catalog while live mutation hooks borrow it.
class HalTintaLearnerPreparation final {
 public:
  explicit HalTintaLearnerPreparation(const Identity& course)
      : course(course), reader(course, scratch), source(packStorage), catalog(pack, source) {}
  ~HalTintaLearnerPreparation() {
    pack.close();
    if (!packStorage.close()) failure("pack close");
  }
  bool run(tinta::core::StateStore& store, tinta::core::Profile& profile, const tinta::core::pack::Pack& appPack) {
    snapshotReady = false;
    const auto loaded = reader.load(TintaDerivedRecord::Receipt, receipt);
    // Legacy startup remains separate from canonical authority recovery.
    if (loaded == TintaDerivedRecordLoad::Missing) return allowLegacyTintaWithoutReceipt(course);
    if (loaded != TintaDerivedRecordLoad::Loaded) return failure("baseline receipt");
    bool present = false;
    if (readCourseBinding(transfer, COURSE_BINDING_PATH, scratch, installed, present) != CourseBindingResult::Ok ||
        !present || installed.logicalIdentity != course ||
        !transfer.verify(ACTIVE_COURSE_PATH, installed.length, installed.contentHash, scratch) ||
        provisionIdentity(identities, identity) != IdentityResult::Ok || !identities.randomIdentity(snapshot))
      return failure("installed course or storage identity");
    if (!packStorage.open(ACTIVE_COURSE_PATH) || !source.attach() ||
        validateCourseCandidate(pack, source, scratch) != CourseValidationResult::Ok || !catalog.prepare(scratch))
      return failure("course validation");
    {
      auto recovery = makeUniqueNoThrow<HalTintaIncrementalRecovery>(course);
      if (!recovery) return failure("OOM: incremental recovery owner");
      const auto result = recovery->run(identity.storageGeneration, installed.contentHash, catalog, 0, snapshot);
      if ((result != TintaIncrementalRecoveryResult::Unchanged && result != TintaIncrementalRecoveryResult::Rebuilt) ||
          !recovery->journalFrontier())
        return failure("incremental recovery");
      frontier = *recovery->journalFrontier();
    }
    {
      auto proof = makeUniqueNoThrow<HalTintaDerivedJournalProof>(course, identity.storageGeneration,
                                                                  installed.contentHash, catalog, scratch);
      auto preparation = makeUniqueNoThrow<HalTintaNativeDerivedPreparation>(course, proof.get(),
                                                                             HalTintaDerivedJournalProof::callback);
      if (!proof || !preparation) return failure("OOM: native preparation owners");
      if (preparation->run(store, profile, appPack, catalog, identity.storageGeneration, installed.contentHash,
                           frontier) != TintaNativePreparationResult::Prepared)
        return failure("native preparation");
    }
    {
      auto audit = makeUniqueNoThrow<HalJournalCausalAuditSession>();
      auto preferences = makeUniqueNoThrow<TintaPreferenceResolution>();
      if (!audit || !preferences) return failure("OOM: preference resolution owners");
      if (!audit->run() || audit->resolveTintaPreferences(*preferences) != TintaJournalResult::Ok)
        return failure("preference authority or conflict");
      if (!persistResolvedTintaPreferences(store, profile, preferences->bodies()))
        return failure("preference application");
    }
    if (reader.load(TintaDerivedRecord::Receipt, receipt) != TintaDerivedRecordLoad::Loaded ||
        !sessionManifest.decode(receipt) ||
        !sessionManifest.matches(course, identity.storageGeneration, installed.contentHash, frontier) ||
        !hashing.digest(receipt, sessionDigest))
      return failure("session snapshot digest");
    snapshotReady = true;
    return true;
  }

  const Digest* sessionSnapshot() const { return snapshotReady ? &sessionDigest : nullptr; }
  const Identity& storageGeneration() const { return identity.storageGeneration; }
  const Digest& resource() const { return installed.contentHash; }
  TintaSubjectCatalog& subjects() { return catalog; }

 private:
  static bool failure(const char* reason) {
    LOG_ERR("COMPANION", "Tinta learner preparation failed: %s", reason);
    return false;
  }
  Identity course, snapshot{};
  IdentityState identity;
  HalIdentityStorage identities;
  HalTransferStorage transfer;
  ContentManifest installed;
  std::array<uint8_t, 512> scratch{};
  std::array<uint8_t, TINTA_DERIVED_MANIFEST_SIZE> receipt{};
  HalTintaDerivedRecordReader reader;
  TintaDerivedManifestView sessionManifest;
  HalInventoryIndexStorage packStorage;
  StoredCourseSource source;
  tinta::core::pack::Pack pack;
  TintaPackSubjectCatalog catalog;
  HalTintaJournalStorage hashing;
  Digest frontier{}, sessionDigest{};
  bool snapshotReady = false;
};
}  // namespace companion
#endif
