#pragma once

#if LILA_TINTA
#include "CompanionJournalMergeIntent.h"
#include "CompanionJournalMergeReadiness.h"
#include "HalCourseStateMigration.h"
#include "HalLegacyTintaBackupSession.h"
#include "HalTintaDerivedFileVerification.h"
#include "HalTintaDerivedStartupRecovery.h"
#include "HalTintaIncrementalRecovery.h"
#include "HalTintaMigrationAdmissions.h"

namespace companion {
// Checked heap owner for immutable installed-pack handles during commit validation.
class HalTintaJournalMergeCommitContext {
 public:
  HalTintaJournalMergeCommitContext() : source(packStorage), subjects(pack, source) {}
  ~HalTintaJournalMergeCommitContext() {
    pack.close();
    if (!packStorage.close()) failure("pack close");
  }
  bool prepare(std::span<const uint8_t> declaration, const Identity& generation) {
    ready = false;
    if (!decodeJournalMergeIntent(declaration, merge) || merge.generation != generation)
      return failure("merge generation");
    return prepareBaseline(generation);
  }
  size_t readinessReply(std::span<const uint8_t> request, const Identity& generation, std::span<uint8_t> reply) {
    ready = false;
    if (reply.size() < JOURNAL_MERGE_READINESS_REPLY_SIZE ||
        !decodeJournalMergeReadinessRequest(request, merge.generation, merge.previous) ||
        merge.generation != generation)
      return 0;
    auto result = JournalMergeReadiness::Unavailable;
    {
      // Audit scratch exceeds the stack budget; release it before opening the pack catalog.
      auto audit = makeUniqueNoThrow<HalJournalCausalAuditSession>();
      if (!audit) {
        failure("OOM: merge readiness audit");
        return 0;
      }
      Digest frontier{};
      if (!audit->run(&frontier) || frontier != merge.previous.frontier ||
          audit->recordCount() != merge.previous.count || audit->recordSize() != merge.previous.recordSize)
        return encodeJournalMergeReadinessReply(result, reply.first(JOURNAL_MERGE_READINESS_REPLY_SIZE))
                   ? JOURNAL_MERGE_READINESS_REPLY_SIZE
                   : 0;
    }
    bool present = false;
    if (installedCourse(installed.logicalIdentity, present)) {
      if (!present) {
        result = JournalMergeReadiness::NoCourse;
      } else {
        reader.emplace(installed.logicalIdentity, scratch);
        const auto receipt = reader->load(TintaDerivedRecord::Receipt, baselineBytes);
        if (receipt == TintaDerivedRecordLoad::Missing)
          result = JournalMergeReadiness::MigrationRequired;
        else if (receipt == TintaDerivedRecordLoad::Loaded && prepareBaseline(generation))
          result = JournalMergeReadiness::Ready;
      }
    }
    return encodeJournalMergeReadinessReply(result, reply.first(JOURNAL_MERGE_READINESS_REPLY_SIZE))
               ? JOURNAL_MERGE_READINESS_REPLY_SIZE
               : 0;
  }
  const Identity* course() const { return ready ? &installed.logicalIdentity : nullptr; }
  TintaSubjectCatalog* catalog() { return ready ? &subjects : nullptr; }
  bool installedCourse(Identity& output, bool& present) {
    if (readCourseBinding(transfer, COURSE_BINDING_PATH, scratch, installed, present) != CourseBindingResult::Ok)
      return failure("course lookup");
    if (present) output = installed.logicalIdentity;
    return true;
  }
  size_t migrationAdmissionReply(std::span<const uint8_t> request, const IdentityState& identity, const Identity& owner,
                                 std::span<uint8_t> reply) {
    ready = false;
    if (reply.size() < 24 || !decodeTintaMigrationAdmission(request, migration)) return 0;
    auto result = TintaJournalResult::Conflict;
    if (migration.reader == identity.device && migration.merge.generation == identity.storageGeneration &&
        migration.merge.owner == owner)
      result = admitMigration(migration, identity, owner) ? TintaJournalResult::Ok : TintaJournalResult::Invalid;
    std::fill_n(reply.begin(), 24, uint8_t{0});
    reply[0] = 1;
    reply[1] = static_cast<uint8_t>(result);
    std::copy(migration.merge.transaction.begin(), migration.merge.transaction.end(), reply.begin() + 4);
    tinta_body_detail::write(reply, 20, migration.merge.previous.count, 4);
    return 24;
  }
  bool prepareMigration(std::span<const uint8_t> declaration, const IdentityState& identity, const Identity& owner) {
    ready = false;
    if (!decodeJournalMergeIntent(declaration, merge) || merge.generation != identity.storageGeneration ||
        merge.owner != owner)
      return failure("migration commit identity");
    bool present = false;
    if (!installedCourse(installed.logicalIdentity, present) || !present) return failure("migration commit course");
    {
      auto hashing = makeUniqueNoThrow<HalTintaJournalStorage>();
      if (!hashing) return failure("OOM: migration commit hashing");
      auto records = makeUniqueNoThrow<HalTintaMigrationAdmissions>(*hashing);
      if (!records) return failure("OOM: migration commit admission");
      if (records->load(installed.logicalIdentity, merge, migration) != TintaMigrationAdmissionResult::Ok)
        return failure("migration commit admission");
    }
    return admitMigration(migration, identity, owner);
  }
  // Caller authenticates the peer and excludes native/journal writers until commit.
  bool admitMigration(const TintaMigrationAdmission& admission, const IdentityState& identity,
                      const Identity& authenticatedOwner) {
    ready = false;
    if (!validTintaMigrationAdmission(admission) || admission.reader != identity.device ||
        admission.merge.generation != identity.storageGeneration || admission.merge.owner != authenticatedOwner)
      return failure("migration identity/owner");
    bool present = false;
    if (!installedCourse(installed.logicalIdentity, present) || !present ||
        installed.logicalIdentity != admission.course || installed.contentHash != admission.resource ||
        !transfer.verify(ACTIVE_COURSE_PATH, installed.length, installed.contentHash, scratch))
      return failure("migration installed resource");
    reader.emplace(admission.course, scratch);
    if (reader->load(TintaDerivedRecord::Receipt, baselineBytes) != TintaDerivedRecordLoad::Missing)
      return failure("migration already canonical or unavailable");
    if (!packStorage.open(ACTIVE_COURSE_PATH) || !source.attach() ||
        validateCourseCandidate(pack, source, scratch) != CourseValidationResult::Ok || !subjects.prepare(scratch))
      return failure("migration pack catalog");
    {
      auto audit = makeUniqueNoThrow<HalJournalCausalAuditSession>();
      if (!audit) return failure("OOM: migration journal audit");
      Digest frontier{};
      bool containsCourse = false;
      if (!audit->run(&frontier) || frontier != admission.merge.previous.frontier ||
          audit->recordCount() != admission.merge.previous.count ||
          audit->recordSize() != admission.merge.previous.recordSize ||
          !audit->containsTintaCourse(admission.course, containsCourse) || containsCourse)
        return failure("migration old journal");
    }
    auto hashing = makeUniqueNoThrow<HalTintaJournalStorage>();
    if (!hashing) return failure("OOM: migration hashing storage");
    {
      auto backup = makeUniqueNoThrow<HalLegacyTintaBackupSession>();
      if (!backup) return failure("OOM: migration backup proof");
      if (!backup->prepareCourse(admission.reader, admission.merge.generation, admission.course,
                                 admission.backupTransaction, true, true) ||
          !backup->recover() || !backup->verifyMigrationBackup(admission, *hashing))
        return failure("migration reviewed backup");
    }
    auto admissions = makeUniqueNoThrow<HalTintaMigrationAdmissions>(*hashing);
    if (!admissions) return failure("OOM: migration admission store");
    if (admissions->persist(admission) != TintaMigrationAdmissionResult::Ok)
      return failure("migration admission publication");
    ready = true;
    return true;
  }
  // Run after pending/received merge publication, before enabling radios.
  bool reconcileLocalHistory(const Identity& course, const Identity& generation, IdentityStorage& identities) {
    ready = false;
    bool present = false;
    if (!installedCourse(installed.logicalIdentity, present) || !present || installed.logicalIdentity != course)
      return failure("local history course");
    if (!transfer.verify(ACTIVE_COURSE_PATH, installed.length, installed.contentHash, scratch) ||
        !packStorage.open(ACTIVE_COURSE_PATH) || !source.attach() ||
        validateCourseCandidate(pack, source, scratch) != CourseValidationResult::Ok || !subjects.prepare(scratch) ||
        !courseStateDirectory(course, path) || !Storage.ensureDirectoryExists("/tinta/courses") ||
        !Storage.ensureDirectoryExists(path.data()))
      return failure("local history pack/state directory");
    reader.emplace(course, scratch);
    const auto loaded = reader->load(TintaDerivedRecord::Receipt, baselineBytes);
    if (loaded != TintaDerivedRecordLoad::Loaded && loaded != TintaDerivedRecordLoad::Missing)
      return failure("local history baseline");
    if (loaded == TintaDerivedRecordLoad::Missing) {
      Identity selected{};
      bool bound = false;
      if (!selectActiveCourseState(transfer, scratch, selected, bound) || !bound || selected != course)
        return failure("initial course state namespace");
    }
    Identity snapshot{};
    if (!identities.randomIdentity(snapshot)) return failure("local history snapshot identity");
    auto recovery = makeUniqueNoThrow<HalTintaIncrementalRecovery>(course);
    if (!recovery) return failure("OOM: local history recovery");
    if (loaded == TintaDerivedRecordLoad::Missing &&
        !recovery->initialize(generation, installed.contentHash, subjects, snapshot))
      return failure("initial alpha baseline");
    const auto result = recovery->run(generation, installed.contentHash, subjects, 0, snapshot);
    return result == TintaIncrementalRecoveryResult::Unchanged || result == TintaIncrementalRecoveryResult::Rebuilt ||
           failure("local history reconciliation");
  }

 private:
  bool prepareBaseline(const Identity& generation) {
    bool present = false;
    if (readCourseBinding(transfer, COURSE_BINDING_PATH, scratch, installed, present) != CourseBindingResult::Ok ||
        !present || !transfer.verify(ACTIVE_COURSE_PATH, installed.length, installed.contentHash, scratch))
      return failure("installed course binding/hash");
    reader.emplace(installed.logicalIdentity, scratch);
    if (reader->load(TintaDerivedRecord::Receipt, baselineBytes) != TintaDerivedRecordLoad::Loaded ||
        !baseline.decode(baselineBytes) ||
        !baseline.matches(installed.logicalIdentity, generation, installed.contentHash, merge.previous.frontier) ||
        baseline.revision() == UINT64_MAX)
      return failure("authoritative baseline");
    for (unsigned at = 0; at < 5; ++at) {
      const auto kind = static_cast<TintaDerivedFile>(at);
      if (!tintaDerivedFilePath(installed.logicalIdentity, kind, TintaDerivedRole::Active, path))
        return failure("learner path");
      HalFile file;
      if (!Storage.openFileForRead("COMPANION", path.data(), file) ||
          verifyTintaDerivedFileReceipt(file, baseline, kind, scratch) != TintaDerivedVerification::Verified)
        return failure("baseline file changed");
    }
    if (!packStorage.open(ACTIVE_COURSE_PATH) || !source.attach() ||
        validateCourseCandidate(pack, source, scratch) != CourseValidationResult::Ok || !subjects.prepare(scratch))
      return failure("installed pack catalog");
    ready = true;
    return true;
  }
  static bool failure(const char* stage) {
    LOG_ERR("COMPANION", "Tinta merge commit validation failed: %s", stage);
    return false;
  }
  HalTransferStorage transfer;
  ContentManifest installed;
  JournalMergeIntent merge;
  TintaMigrationAdmission migration;
  std::array<uint8_t, 512> scratch{};
  std::array<uint8_t, TINTA_DERIVED_MANIFEST_SIZE> baselineBytes{};
  std::optional<HalTintaDerivedRecordReader> reader;
  TintaDerivedManifestView baseline;
  std::array<char, COURSE_STATE_PATH_SIZE> path{};
  HalInventoryIndexStorage packStorage;
  StoredCourseSource source;
  tinta::core::pack::Pack pack;
  TintaPackSubjectCatalog subjects;
  bool ready = false;
};
}  // namespace companion
#endif
