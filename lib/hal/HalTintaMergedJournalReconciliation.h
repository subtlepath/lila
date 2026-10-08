#pragma once

#if LILA_TINTA
#include "HalJournalMergeRecordStore.h"
#include "HalTintaDerivedFileVerification.h"
#include "HalTintaDerivedStartupRecovery.h"
#include "HalTintaInitialMigrationReconciliation.h"
#include "HalTintaReplayExport.h"
#include "HalTintaReplaySession.h"

namespace companion {
// Short-lived checked heap owner, released before opening the learner UI.
class HalTintaMergedJournalReconciliation {
 public:
  HalTintaMergedJournalReconciliation(const Identity& course, IdentityStorage& identities)
      : course(course), identities(identities), reader(course, scratch), source(packStorage), catalog(pack, source) {
    static constexpr char PREFIX[] = "/.crosspoint/companion/journal-merge-applied-";
    static constexpr char HEX_DIGITS[] = "0123456789abcdef";
    static constexpr size_t PREFIX_SIZE = sizeof(PREFIX) - 1;
    static_assert(PREFIX_SIZE + 38 <= 96);
    std::copy_n(PREFIX, PREFIX_SIZE, appliedPath.begin());
    for (size_t at = 0; at < course.size(); ++at) {
      appliedPath[PREFIX_SIZE + 2 * at] = HEX_DIGITS[course[at] >> 4];
      appliedPath[PREFIX_SIZE + 2 * at + 1] = HEX_DIGITS[course[at] & 15];
    }
    appliedPath[PREFIX_SIZE + 32] = 0;
    std::copy_n(appliedPath.begin(), PREFIX_SIZE + 32, appliedStage.begin());
    std::copy_n("-next", 6, appliedStage.begin() + PREFIX_SIZE + 32);
  }
  ~HalTintaMergedJournalReconciliation() {
    pack.close();
    if (!packStorage.close()) failure("pack close");
  }
  bool run() {
    if (!tinta_body_detail::nonzero(course)) return failure("course identity");
    const auto receipt = merges.load(merge);
    if (receipt == JournalMigrationPresence::Missing) return true;
    if (receipt != JournalMigrationPresence::Present) return failure("merge receipt");
    const auto processed = applied.load(previousMerge);
    if (processed == JournalMigrationPresence::IoError) return failure("applied receipt");
    if (processed == JournalMigrationPresence::Present && previousMerge == merge) return true;
    bool changed = false;
    const bool inspected = inspectSuffix(changed);
    const bool closed = journalStorage.close();
    if (!inspected || !closed) return failure("merged journal suffix");
    if (provisionIdentity(identities, identity) != IdentityResult::Ok || identity.storageGeneration != merge.generation)
      return failure("storage generation");
    if (!changed) {
      auto audit = makeUniqueNoThrow<HalJournalCausalAuditSession>();
      if (!audit) return failure("OOM: audit workspace");
      Digest frontier{};
      if (!audit->run(&frontier) || frontier != merge.merged.frontier || audit->recordCount() != merge.merged.count ||
          audit->recordSize() != merge.merged.recordSize)
        return failure("merged frontier");
      audit.reset();
      auto initial = makeUniqueNoThrow<HalTintaInitialMigrationReconciliation>();
      if (!initial) return failure("OOM: migration admission lookup");
      const auto migration = initial->lookup(course, merge);
      if (migration == TintaMigrationAdmissionResult::Missing) return applied.persist(merge);
      if (migration != TintaMigrationAdmissionResult::Ok) return failure("migration admission lookup");
    }
    bool present = false;
    if (readCourseBinding(transfer, COURSE_BINDING_PATH, scratch, installed, present) != CourseBindingResult::Ok ||
        !present || installed.logicalIdentity != course ||
        !transfer.verify(ACTIVE_COURSE_PATH, installed.length, installed.contentHash, scratch))
      return failure("installed course binding");
    const auto baseline = reader.load(TintaDerivedRecord::Receipt, previousBytes);
    if (baseline == TintaDerivedRecordLoad::Missing) {
      if (!packStorage.open(ACTIVE_COURSE_PATH) || !source.attach() ||
          validateCourseCandidate(pack, source, scratch) != CourseValidationResult::Ok || !catalog.prepare(scratch))
        return failure("initial migration course validation");
      auto initial = makeUniqueNoThrow<HalTintaInitialMigrationReconciliation>();
      if (!initial) return failure("OOM: initial migration reconciliation");
      return (initial->run(course, identity, installed.contentHash, merge, catalog) && applied.persist(merge)) ||
             failure("initial migration reconciliation");
    }
    if (baseline != TintaDerivedRecordLoad::Loaded) return failure("missing authoritative baseline");
    TintaDerivedManifestView previous;
    if (!previous.decode(previousBytes)) return failure("baseline decode");
    const bool alreadyPublished =
        previous.matches(course, identity.storageGeneration, installed.contentHash, merge.merged.frontier);
    if (!alreadyPublished &&
        (!previous.matches(course, identity.storageGeneration, installed.contentHash, merge.previous.frontier) ||
         previous.revision() == UINT64_MAX))
      return failure("baseline binding/revision");
    for (unsigned at = 0; at < 5; ++at) {
      const auto kind = static_cast<TintaDerivedFile>(at);
      if (!tintaDerivedFilePath(course, kind, TintaDerivedRole::Active, learnerPath)) return failure("learner path");
      HalFile file;
      if (!Storage.openFileForRead("COMPANION", learnerPath.data(), file) ||
          verifyTintaDerivedFileReceipt(file, previous, kind, scratch) != TintaDerivedVerification::Verified)
        return failure("baseline file changed");
    }
    if (alreadyPublished) return applied.persist(merge);
    if (!packStorage.open(ACTIVE_COURSE_PATH) || !source.attach() ||
        validateCourseCandidate(pack, source, scratch) != CourseValidationResult::Ok || !catalog.prepare(scratch))
      return failure("course validation");
    // Replay/export own fixed buffers and retained handles beyond the task-local budget.
    auto replay = makeUniqueNoThrow<HalTintaReplaySession>();
    auto output = makeUniqueNoThrow<HalTintaReplayExport>();
    if (!replay || !output) return failure("OOM: replay/export workspace");
    if (!replay->run(course, catalog) || *replay->journalFrontier() != merge.merged.frontier ||
        !output->run(*replay->workingStore(), course, previous.studyDay(), scratch) ||
        !output->manifest(identity.storageGeneration, installed.contentHash, merge.merged.frontier, merge.transaction,
                          previous.revision() + 1, manifestBytes) ||
        !replay->workingStore()->close())
      return failure("derived replay/export");
    output.reset();
    replay.reset();
    if (publishProvenTintaDerived(course, identity.storageGeneration, installed.contentHash, catalog, manifestBytes,
                                  scratch, previousBytes) != TintaPublicationResult::Ok)
      return failure("derived publication");
    return applied.persist(merge);
  }

 private:
  bool inspectSuffix(bool& changed) {
    if (journal.open() != TintaJournalResult::Ok || journal.count() != merge.merged.count ||
        journal.recordSize() != merge.merged.recordSize)
      return false;
    for (uint32_t at = merge.previous.count; at < merge.merged.count; ++at) {
      if (journal.read(at) != TintaJournalResult::Ok) return false;
      if (journal.event().kind < EventKind::Review) continue;
      if (!decodeTintaBody(journal.body(), body)) return false;
      if (body.course == course) changed = true;
    }
    return true;
  }
  static bool failure(const char* stage) {
    LOG_ERR("COMPANION", "Merged Tinta reconciliation failed: %s", stage);
    return false;
  }
  Identity course;
  IdentityStorage& identities;
  HalJournalMergeRecordStore merges{JournalMergeRecord::Receipt};
  std::array<char, 96> appliedPath{}, appliedStage{};
  HalJournalMergeRecordStore applied{appliedPath.data(), appliedStage.data(), true};
  JournalMergeIntent merge, previousMerge;
  std::array<uint8_t, 512> scratch{};
  std::array<uint8_t, TINTA_DERIVED_MANIFEST_SIZE> previousBytes{}, manifestBytes{};
  HalTintaDerivedRecordReader reader;
  HalTransferStorage transfer;
  IdentityState identity;
  ContentManifest installed;
  std::array<char, COURSE_STATE_PATH_SIZE> learnerPath{};
  HalInventoryIndexStorage packStorage;
  StoredCourseSource source;
  tinta::core::pack::Pack pack;
  TintaPackSubjectCatalog catalog;
  HalTintaJournalStorage journalStorage;
  std::array<uint8_t, TintaJournal::EXTENDED_RECORD_SIZE> journalScratch{};
  TintaJournal journal{journalStorage, journalScratch};
  TintaBody body;
};
inline bool reconcileBoundTintaJournalMerge(const Identity& course, HalCompanionFileLookup& lookup) {
  const auto presence = lookup.inspect(HalJournalMergeRecordStore::RECEIPT);
  if (presence == CompanionFilePresence::Missing) return true;
  if (presence == CompanionFilePresence::Error) return false;
  HalIdentityStorage identities;
  auto reconciliation = makeUniqueNoThrow<HalTintaMergedJournalReconciliation>(course, identities);
  if (!reconciliation) {
    LOG_ERR("COMPANION", "OOM: merged Tinta reconciliation workspace");
    return false;
  }
  return reconciliation->run();
}
}  // namespace companion
#endif
