#include <gtest/gtest.h>

#include <fstream>
#include <iterator>

#include "../tinta/fakes.h"
#include "CompanionJournalMigration.h"
#include "HalBookmarkIdentityStage.h"
#include "HalJournalCausalAuditSession.h"
#include "HalJournalFormatInventory.h"
#include "HalJournalMergeCandidateSession.h"
#include "HalJournalMergePublicationStorage.h"
#include "HalJournalMergeReceiveSession.h"
#include "HalJournalMergeRecordStore.h"
#include "HalJournalMigrationIntentStore.h"
#include "HalJournalMigrationPublicationStorage.h"
#include "HalJournalMigrationSession.h"
#include "HalJournalReplayVisits.h"
#include "HalJournalStartupRecovery.h"
#include "HalJournalStateQuery.h"
#include "HalLegacyBackupExchange.h"
#include "HalLegacyTintaBackupCapture.h"
#include "HalLegacyTintaBackupCopy.h"
#include "HalLegacyTintaBackupManifestStore.h"
#include "HalLegacyTintaBackupSession.h"
#include "HalLegacyTintaJournalReader.h"
#include "HalTintaApplicationAcknowledge.h"
#include "HalTintaApplicationReceiptStore.h"
#include "HalTintaApplicationReceipts.h"
#include "HalTintaAuthorityCheckpointStore.h"
#include "HalTintaAuthorityCheckpoints.h"
#include "HalTintaAuthorityRetention.h"
#include "HalTintaDerivedJournalProof.h"
#include "HalTintaIncrementalRecovery.h"
#include "HalTintaJournalStorage.h"
#include "HalTintaLegacyAdmission.h"
#include "HalTintaMigrationAdmissionStore.h"
#include "HalTintaMigrationAdmissions.h"
#include "HalTintaPreferenceApplication.h"
#include "HalTintaProvenPublication.h"
#include "HalTintaReplayCompletionExport.h"
#include "HalTintaReplayDayExport.h"
#include "HalTintaReplayExport.h"
#include "HalTintaReplayItemExport.h"
#include "HalTintaReplaySession.h"
#include "HalTintaReplayStore.h"
#include "lib/Tinta/src/core/srs/Bytes.h"

using namespace companion;

namespace {
JournalMergeIntent mergeIntent() {
  JournalMergeIntent intent;
  intent.generation.fill(1);
  intent.transaction.fill(2);
  intent.owner.fill(3);
  intent.previous = {1, 512, {}};
  intent.previous.frontier.fill(4);
  intent.merged = {2, 1024, {}};
  intent.merged.frontier.fill(5);
  return intent;
}
}  // namespace

TEST(HalTintaJournalStorageTest, MergeIntentPersistsReadbackAndRejectsConflictingOrCorruptRecords) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  HalJournalMergeRecordStore records;
  const auto expected = mergeIntent();
  JournalMergeIntent loaded = expected;
  EXPECT_EQ(records.load(loaded), JournalMigrationPresence::Missing);
  ASSERT_TRUE(records.persist(expected));
  EXPECT_EQ(state.files[HalJournalMergeRecordStore::INTENT].size(), JOURNAL_MERGE_INTENT_SIZE);
  ASSERT_EQ(records.load(loaded), JournalMigrationPresence::Present);
  EXPECT_EQ(loaded, expected);
  const auto before = state.files;
  ASSERT_TRUE(records.persist(expected));
  EXPECT_EQ(state.files, before);
  auto different = expected;
  different.transaction[0] ^= 1;
  EXPECT_FALSE(records.persist(different));
  EXPECT_FALSE(records.clear(different));
  EXPECT_EQ(state.files, before);
  state.files[HalJournalMergeRecordStore::INTENT][10] ^= 1;
  EXPECT_EQ(records.load(loaded), JournalMigrationPresence::IoError);
  EXPECT_EQ(loaded, expected);
  EXPECT_FALSE(records.clear(expected));
}

TEST(HalTintaJournalStorageTest, MergeRecordsRecoverLostRenameAcknowledgmentsAndKeepOldReceiptOnFailedSync) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  const auto expected = mergeIntent();
  HalJournalMergeRecordStore intents;
  state.failRenameAfterSource = HalJournalMergeRecordStore::INTENT_STAGE;
  EXPECT_FALSE(intents.persist(expected));
  ASSERT_TRUE(intents.persist(expected));
  HalJournalMergeRecordStore receipts(JournalMergeRecord::Receipt);
  state.failRenameAfterSource.clear();
  ASSERT_TRUE(receipts.persist(expected));
  auto next = expected;
  next.transaction[0] ^= 1;
  const auto previousReceipt = state.files[HalJournalMergeRecordStore::RECEIPT];
  state.failSync = true;
  EXPECT_FALSE(receipts.persist(next));
  EXPECT_EQ(state.files[HalJournalMergeRecordStore::RECEIPT], previousReceipt);
  state.failSync = false;
  state.failRenameAfterSource = HalJournalMergeRecordStore::RECEIPT_STAGE;
  EXPECT_FALSE(receipts.persist(next));
  ASSERT_TRUE(receipts.persist(next));
  JournalMergeIntent loaded;
  ASSERT_EQ(receipts.load(loaded), JournalMigrationPresence::Present);
  EXPECT_EQ(loaded, next);
  ASSERT_TRUE(intents.clear(expected));
  EXPECT_EQ(intents.load(loaded), JournalMigrationPresence::Missing);
  EXPECT_TRUE(state.files.contains(HalJournalMergeRecordStore::RECEIPT));
}

TEST(HalTintaJournalStorageTest, FormatInventoryIncludesMergeCandidateAndBackupWithoutChangingFiles) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
  for (const auto location : {TintaJournalLocation::MergeCandidate, TintaJournalLocation::MergeBackup}) {
    HalTintaJournalStorage storage(location);
    std::array<uint8_t, 1024> scratch{};
    TintaJournal journal(storage, scratch);
    ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
  }
  HalJournalFormatInventory formats;
  uint8_t versions = 99;
  const auto persisted = state.files;
  ASSERT_TRUE(formats.inspect(versions));
  EXPECT_EQ(versions, 4);
  EXPECT_EQ(state.files, persisted);
  state.files[MERGE_BACKUP_TINTA_JOURNAL_PATHS.headerB][0] ^= 1;
  const auto corrupted = state.files;
  EXPECT_FALSE(formats.inspect(versions));
  EXPECT_EQ(versions, 4);
  EXPECT_EQ(state.files, corrupted);
}

TEST(HalTintaJournalStorageTest, MergePublicationProvesRetentionAndRecoversRealDirectoryRename) {
  auto& state = inventory_hal_test::state;
  for (const bool changedPrefix : {false, true}) {
    state = {};
    state.enumerateFileMap = true;
    ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
    for (const auto location : {TintaJournalLocation::Active, TintaJournalLocation::MergeCandidate}) {
      HalTintaJournalStorage storage(location);
      std::array<uint8_t, 1024> scratch{};
      TintaJournal journal(
          storage, location == TintaJournalLocation::Active ? std::span(scratch).first(512) : std::span(scratch));
      ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
      TintaBody body;
      body.kind = EventKind::Star;
      body.course.fill(7);
      body.uid = 1;
      body.enabled = changedPrefix && location == TintaJournalLocation::MergeCandidate;
      std::array<uint8_t, MAX_TINTA_BODY_SIZE> bytes{};
      const auto length = encodeTintaBody(body, bytes);
      SyncEvent event;
      event.identity.origin.fill(1);
      event.identity.epoch = event.identity.sequence = 1;
      event.storageGeneration.fill(2);
      event.resource.fill(3);
      event.kind = EventKind::Star;
      ASSERT_TRUE(storage.digest(std::span(bytes).first(length), event.bodyHash));
      ASSERT_EQ(journal.append(event, std::span(bytes).first(length)), TintaJournalResult::Ok);
      if (location == TintaJournalLocation::MergeCandidate) {
        event.identity.sequence = 2;
        ASSERT_EQ(journal.append(event, std::span(bytes).first(length)), TintaJournalResult::Ok);
      }
    }
    JournalMergeIntent expected = mergeIntent();
    {
      HalJournalCausalAuditSession active, candidate(TintaJournalLocation::MergeCandidate);
      ASSERT_TRUE(active.run(&expected.previous.frontier));
      expected.previous.count = active.recordCount();
      expected.previous.recordSize = active.recordSize();
      ASSERT_TRUE(candidate.run(&expected.merged.frontier));
      expected.merged.count = candidate.recordCount();
      expected.merged.recordSize = candidate.recordSize();
    }
    HalJournalMergePublicationStorage publication;
    const auto oldEvents = state.files[TINTA_JOURNAL_EVENTS];
    const auto mergedEvents = state.files[MERGE_TINTA_JOURNAL_PATHS.events];
    if (changedPrefix) {
      EXPECT_FALSE(publication.authorize(expected, expected.generation));
      EXPECT_FALSE(state.files.contains(HalJournalMergeRecordStore::INTENT));
      EXPECT_EQ(state.files[TINTA_JOURNAL_EVENTS], oldEvents);
      EXPECT_EQ(state.files[MERGE_TINTA_JOURNAL_PATHS.events], mergedEvents);
      EXPECT_EQ(state.directoryRenames, 0u);
      continue;
    }
    ASSERT_TRUE(publication.authorize(expected, expected.generation));
    state.failRenameAfterSource = TINTA_JOURNAL_DIRECTORY;
    EXPECT_EQ(recoverJournalMerge(publication, expected.generation), JournalMigrationPublicationResult::IoError);
    state.failRenameAfterSource.clear();
    EXPECT_EQ(recoverJournalMerge(publication, expected.generation), JournalMigrationPublicationResult::Complete);
    EXPECT_EQ(state.files[TINTA_JOURNAL_EVENTS], mergedEvents);
    EXPECT_EQ(state.files[MERGE_BACKUP_TINTA_JOURNAL_PATHS.events], oldEvents);
    EXPECT_FALSE(state.files.contains(HalJournalMergeRecordStore::INTENT));
    HalJournalMergeRecordStore receipt(JournalMergeRecord::Receipt);
    JournalMergeIntent persisted;
    ASSERT_EQ(receipt.load(persisted), JournalMigrationPresence::Present);
    EXPECT_EQ(persisted, expected);
    EXPECT_TRUE(publication.complete(expected));
    EXPECT_EQ(recoverJournalMerge(publication, expected.generation), JournalMigrationPublicationResult::NoPending);
  }
}

TEST(HalTintaJournalStorageTest, ReceiveDispatcherRejectsForeignOwnerGenerationAndTransactionBeforeWrites) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
  {
    HalTintaJournalStorage storage;
    std::array<uint8_t, 1024> scratch{};
    TintaJournal journal(storage, scratch);
    ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
  }
  JournalMergeIntent declaration = mergeIntent();
  {
    HalJournalCausalAuditSession audit;
    ASSERT_TRUE(audit.run(&declaration.previous.frontier));
    declaration.previous.count = audit.recordCount();
    declaration.previous.recordSize = audit.recordSize();
  }
  declaration.merged.count = 1;
  std::array<uint8_t, JOURNAL_MERGE_INTENT_SIZE> intent{};
  ASSERT_TRUE(encodeJournalMergeIntent(declaration, intent));
  std::array<uint8_t, MAX_CONTROL_PAYLOAD> payload{};
  JournalMergeRequestView request{JournalMergeOperation::Begin, declaration.transaction, intent, {}, {}};
  auto length = encodeJournalMergeRequest(request, payload);
  ASSERT_GT(length, 0u);
  HalJournalMergeReceiveSession session;
  EXPECT_EQ(session.receivingDeclaration(), nullptr);
  auto foreign = declaration.owner;
  foreign[0] ^= 1;
  const auto initial = state.files;
  EXPECT_EQ(session.dispatch(std::span(payload).first(length), foreign, declaration.generation, nullptr, nullptr),
            TintaJournalResult::Conflict);
  EXPECT_EQ(state.files, initial);
  EXPECT_EQ(session.dispatch(std::span(payload).first(length), declaration.owner, foreign, nullptr, nullptr),
            TintaJournalResult::Conflict);
  EXPECT_EQ(state.files, initial);
  ASSERT_EQ(
      session.dispatch(std::span(payload).first(length), declaration.owner, declaration.generation, nullptr, nullptr),
      TintaJournalResult::Ok);
  ASSERT_NE(session.receivingDeclaration(), nullptr);
  EXPECT_EQ(*session.receivingDeclaration(), declaration);
  TintaBody body;
  body.kind = EventKind::Star;
  body.course.fill(7);
  body.uid = 1;
  std::array<uint8_t, MAX_TINTA_BODY_SIZE> bytes{};
  const auto bodySize = encodeTintaBody(body, bytes);
  SyncEvent event;
  event.identity.origin.fill(1);
  event.identity.epoch = event.identity.sequence = 1;
  event.storageGeneration = declaration.generation;
  event.resource.fill(3);
  event.kind = EventKind::Star;
  SHA256(bytes.data(), bodySize, event.bodyHash.data());
  std::array<uint8_t, MAX_RECORD_SIZE> envelope{};
  const auto envelopeSize = encodeRecord(event, envelope);
  request = {JournalMergeOperation::Append,
             declaration.transaction,
             {},
             std::span(envelope).first(envelopeSize),
             std::span(bytes).first(bodySize)};
  length = encodeJournalMergeRequest(request, payload);
  const auto receiving = state.files;
  EXPECT_EQ(session.dispatch(std::span(payload).first(length), foreign, declaration.generation, nullptr, nullptr),
            TintaJournalResult::Conflict);
  EXPECT_EQ(state.files, receiving);
  EXPECT_EQ(session.dispatch(std::span(payload).first(length), declaration.owner, foreign, nullptr, nullptr),
            TintaJournalResult::Conflict);
  EXPECT_EQ(state.files, receiving);
  payload[8] ^= 1;
  EXPECT_EQ(
      session.dispatch(std::span(payload).first(length), declaration.owner, declaration.generation, nullptr, nullptr),
      TintaJournalResult::Conflict);
  EXPECT_EQ(state.files, receiving);
  payload[8] ^= 1;
  ASSERT_EQ(
      session.dispatch(std::span(payload).first(length), declaration.owner, declaration.generation, nullptr, nullptr),
      TintaJournalResult::Ok);
  EXPECT_EQ(session.count(), 1u);
  EXPECT_TRUE(session.hasBinding());
  EXPECT_EQ(
      session.dispatch(std::span(payload).first(length), declaration.owner, declaration.generation, nullptr, nullptr),
      TintaJournalResult::Duplicate);
  request = {JournalMergeOperation::Abort, declaration.transaction, intent, {}, {}};
  length = encodeJournalMergeRequest(request, payload);
  ASSERT_EQ(
      session.dispatch(std::span(payload).first(length), declaration.owner, declaration.generation, nullptr, nullptr),
      TintaJournalResult::Ok);
  EXPECT_EQ(state.files[TINTA_JOURNAL_EVENTS], initial.at(TINTA_JOURNAL_EVENTS));
  EXPECT_FALSE(state.files.contains(MERGE_TINTA_JOURNAL_PATHS.events));
  EXPECT_FALSE(session.hasBinding());
  EXPECT_EQ(session.receivingDeclaration(), nullptr);
  const auto aborted = state.files;
  EXPECT_EQ(session.dispatch(std::span(payload).first(length), foreign, declaration.generation, nullptr, nullptr),
            TintaJournalResult::Conflict);
  EXPECT_EQ(state.files, aborted);
  HalJournalMergeReceiveSession restarted;
  ASSERT_EQ(
      restarted.dispatch(std::span(payload).first(length), declaration.owner, declaration.generation, nullptr, nullptr),
      TintaJournalResult::Ok);
  EXPECT_EQ(state.files, aborted);
  EXPECT_FALSE(restarted.hasBinding());
  EXPECT_EQ(restarted.receivingDeclaration(), nullptr);
}

TEST(HalTintaJournalStorageTest, LegacyReaderUsesBorrowedHalHandleAndChecksExtentAndCancellation) {
  auto& state = inventory_hal_test::state;
  state = {};
  std::string fixture = TINTA_FRONTIER_FIXTURE;
  fixture.resize(fixture.find_last_of('/') + 1);
  std::ifstream input(fixture + "LegacyTintaJournal-v1.fixture", std::ios::binary);
  ASSERT_TRUE(input.good());
  static constexpr char PATH[] = "/legacy-reviews.log";
  state.files[PATH] = {std::istreambuf_iterator<char>(input), {}};
  const auto original = state.files;
  HalFile file;
  ASSERT_TRUE(Storage.openFileForRead("TEST", PATH, file));
  std::array<uint8_t, 24> scratch{};
  HalLegacyTintaJournalReader reader;
  ASSERT_TRUE(reader.attach(file, scratch));
  LegacyTintaEntry entry;
  for (auto operation : {LegacyTintaOperation::Review, LegacyTintaOperation::Undo, LegacyTintaOperation::Flags}) {
    ASSERT_EQ(reader.next(entry), LegacyTintaReadResult::Record);
    EXPECT_EQ(entry.operation, operation);
  }
  EXPECT_EQ(reader.next(entry), LegacyTintaReadResult::End);
  EXPECT_EQ(reader.next(entry), LegacyTintaReadResult::End);
  EXPECT_EQ(reader.count(), 3u);
  EXPECT_EQ(state.files, original);
  EXPECT_TRUE(file.isOpen());
  ASSERT_TRUE(reader.attach(file, scratch));
  ASSERT_EQ(reader.next(entry), LegacyTintaReadResult::Record);
  state.files[PATH].push_back(0);
  const auto preserved = entry;
  EXPECT_EQ(reader.next(entry), LegacyTintaReadResult::IoError);
  EXPECT_EQ(entry, preserved);
  EXPECT_EQ(reader.next(entry), LegacyTintaReadResult::Unavailable);
  state.files = original;
  bool proceed = true;
  ASSERT_TRUE(reader.attach(file, scratch, [](void* context) { return *static_cast<bool*>(context); }, &proceed));
  proceed = false;
  EXPECT_EQ(reader.next(entry), LegacyTintaReadResult::IoError);
  EXPECT_EQ(entry, preserved);
  EXPECT_TRUE(file.isOpen());
  EXPECT_EQ(state.files, original);
  for (const bool shortRead : {false, true}) {
    ASSERT_TRUE(reader.attach(file, scratch));
    if (shortRead)
      state.shortRead = state.reads + 1;
    else
      state.failRead = state.reads + 1;
    EXPECT_EQ(reader.next(entry), LegacyTintaReadResult::IoError);
    EXPECT_EQ(entry, preserved);
    state.shortRead = state.failRead = 0;
  }
  state.files[PATH].push_back(1);
  ASSERT_TRUE(reader.attach(file, scratch));
  for (unsigned at = 0; at < 3; ++at) ASSERT_EQ(reader.next(entry), LegacyTintaReadResult::Record);
  const auto lastRecord = entry;
  EXPECT_EQ(reader.next(entry), LegacyTintaReadResult::Invalid);
  EXPECT_EQ(entry, lastRecord);
  EXPECT_EQ(reader.next(entry), LegacyTintaReadResult::Unavailable);
}

TEST(HalTintaJournalStorageTest, VerifiedLegacyReaderRejectsWrongHashAndSameExtentChangesBeforeEnd) {
  auto& state = inventory_hal_test::state;
  state = {};
  std::string fixture = TINTA_FRONTIER_FIXTURE;
  fixture.resize(fixture.find_last_of('/') + 1);
  std::ifstream input(fixture + "LegacyTintaJournal-v1.fixture", std::ios::binary);
  ASSERT_TRUE(input.good());
  static constexpr char PATH[] = "/verified-legacy.log";
  state.files[PATH] = {std::istreambuf_iterator<char>(input), {}};
  const auto original = state.files;
  Digest expected{};
  SHA256(state.files[PATH].data(), state.files[PATH].size(), expected.data());
  HalFile file;
  ASSERT_TRUE(Storage.openFileForRead("TEST", PATH, file));
  std::array<uint8_t, 24> scratch{};
  HalLegacyTintaJournalReader reader;
  auto wrong = expected;
  wrong[0] ^= 1;
  EXPECT_FALSE(reader.attachVerified(file, scratch, 48, wrong));
  EXPECT_FALSE(reader.attachVerified(file, scratch, 47, expected));
  EXPECT_EQ(state.files, original);
  ASSERT_TRUE(reader.attachVerified(file, scratch, 48, expected));
  LegacyTintaEntry entry;
  for (unsigned at = 0; at < 3; ++at) ASSERT_EQ(reader.next(entry), LegacyTintaReadResult::Record);
  const auto preserved = entry;
  state.files[PATH][0] ^= 1;
  const auto changed = state.files;
  EXPECT_EQ(reader.next(entry), LegacyTintaReadResult::IoError);
  EXPECT_EQ(entry, preserved);
  EXPECT_EQ(state.files, changed);
  EXPECT_EQ(reader.next(entry), LegacyTintaReadResult::Unavailable);
  state.files = original;
  ASSERT_TRUE(reader.attachVerified(file, scratch, 48, expected));
  for (unsigned at = 0; at < 3; ++at) ASSERT_EQ(reader.next(entry), LegacyTintaReadResult::Record);
  EXPECT_EQ(reader.next(entry), LegacyTintaReadResult::End);
  EXPECT_EQ(reader.next(entry), LegacyTintaReadResult::End);
  EXPECT_EQ(state.files, original);
}

TEST(HalTintaJournalStorageTest, LegacyBackupCopyPreservesOriginalAndResumesLostPublicationAcknowledgment) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  static constexpr char ORIGINAL[] = "/legacy.log";
  static constexpr char ROOT[] = "/legacy-backup";
  static constexpr char CANDIDATE[] = "/legacy-backup/reviews-next";
  static constexpr char BACKUP[] = "/legacy-backup/reviews.log";
  ASSERT_TRUE(Storage.ensureDirectoryExists(ROOT));
  state.files[ORIGINAL] = std::vector<uint8_t>(400, 7);
  const auto original = state.files[ORIGINAL];
  Digest hash{};
  SHA256(original.data(), original.size(), hash.data());
  std::array<uint8_t, 128> scratch{};
  HalLegacyTintaBackupCopy copier(ROOT, scratch);
  EXPECT_FALSE(copier.copy(ORIGINAL, ORIGINAL, BACKUP, original.size(), hash));
  EXPECT_FALSE(copier.copy("/LEGACY-BACKUP/REVIEWS-NEXT", CANDIDATE, BACKUP, original.size(), hash));
  EXPECT_FALSE(copier.copy("/legacy-backup/../legacy.log", CANDIDATE, BACKUP, original.size(), hash));
  EXPECT_FALSE(copier.copy(ORIGINAL, "/legacy-backup/reviews-next.", BACKUP, original.size(), hash));
  state.files[CANDIDATE] = {1, 2, 3};
  const auto unknown = state.files;
  EXPECT_FALSE(copier.copy(ORIGINAL, CANDIDATE, BACKUP, original.size(), hash));
  EXPECT_EQ(state.files, unknown);
  state.files.erase(CANDIDATE);
  state.failRenameAfterSource = CANDIDATE;
  EXPECT_FALSE(copier.copy(ORIGINAL, CANDIDATE, BACKUP, original.size(), hash));
  EXPECT_EQ(state.files[ORIGINAL], original);
  EXPECT_EQ(state.files[BACKUP], original);
  EXPECT_FALSE(state.files.contains(CANDIDATE));
  state.failRenameAfterSource.clear();
  ASSERT_TRUE(copier.copy(ORIGINAL, CANDIDATE, BACKUP, original.size(), hash));
  const auto preserved = state.files;
  ASSERT_TRUE(copier.copy(ORIGINAL, CANDIDATE, BACKUP, original.size(), hash));
  EXPECT_EQ(state.files, preserved);
  state.files[BACKUP][0] ^= 1;
  const auto corrupt = state.files;
  EXPECT_FALSE(copier.copy(ORIGINAL, CANDIDATE, BACKUP, original.size(), hash));
  EXPECT_EQ(state.files, corrupt);
}

TEST(HalTintaJournalStorageTest, LegacyBackupCapturePublishesOnlyCompleteSetAndRetries) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  ASSERT_TRUE(Storage.ensureDirectoryExists("/backup"));
  LegacyTintaBackupManifest manifest;
  manifest.reader[0] = 1;
  manifest.generation[0] = 2;
  manifest.course[0] = 3;
  manifest.transaction[0] = 4;
  std::array<LegacyTintaBackupPaths, LEGACY_TINTA_BACKUP_ROLES> paths{};
  paths[0] = {"/reviews", "/backup/reviews-next", "/backup/reviews"};
  paths[1] = {"/items", "/backup/items-next", "/backup/items"};
  paths[2] = {"/profile", "/backup/profile-next", "/backup/profile"};
  for (size_t at = 0; at < 3; ++at) {
    state.files[paths[at].original] = std::vector<uint8_t>(80, static_cast<uint8_t>(at + 1));
    auto& file = manifest.files[at];
    file.present = true;
    file.length = 80;
    SHA256(state.files[paths[at].original].data(), 80, file.hash.data());
  }
  std::array<uint8_t, LEGACY_TINTA_BACKUP_MANIFEST_SIZE> encoded{};
  ASSERT_TRUE(encodeLegacyTintaBackupManifest(manifest, encoded));
  std::array<uint8_t, 128> scratch{};
  HalLegacyTintaBackupCapture capture("/backup", "/backup/manifest", "/backup/manifest-next", scratch, nullptr, nullptr,
                                      "/backup/intent", "/backup/intent-next");
  const auto originals = state.files;
  size_t hashingCalls = 0;
  HalLegacyTintaBackupCapture bounded(
      "/backup", "/backup/manifest", "/backup/manifest-next", scratch,
      [](void* context) {
        ++*static_cast<size_t*>(context);
        return true;
      },
      &hashingCalls);
  Identity emptyIdentity{};
  EXPECT_FALSE(
      bounded.captureSources(emptyIdentity, manifest.generation, manifest.course, manifest.transaction, paths));
  EXPECT_EQ(hashingCalls, 0);
  state.files["/reviews"].resize(16 * 1024 * 1024 + 1);
  EXPECT_FALSE(
      bounded.captureSources(manifest.reader, manifest.generation, manifest.course, manifest.transaction, paths));
  EXPECT_EQ(hashingCalls, 0);
  EXPECT_EQ(state.files["/reviews"].size(), 16 * 1024 * 1024 + 1);
  EXPECT_FALSE(state.files.contains("/backup/manifest"));
  EXPECT_FALSE(state.files.contains("/backup/reviews-next"));
  state.files["/reviews"] = originals.at("/reviews");
  for (const size_t offset : {size_t{0}, size_t{100}, encoded.size() - 1}) {
    HalLegacyTintaBackupCapture overlapping("/backup", "/backup/manifest", "/backup/manifest-next",
                                            std::span<uint8_t>(encoded).subspan(offset));
    EXPECT_FALSE(overlapping.capture(encoded, paths));
    EXPECT_EQ(state.files, originals);
  }
  auto tableBytes = std::as_writable_bytes(std::span(paths));
  HalLegacyTintaBackupCapture overlappingTable("/backup", "/backup/manifest", "/backup/manifest-next",
                                               {reinterpret_cast<uint8_t*>(tableBytes.data()), tableBytes.size()});
  EXPECT_FALSE(overlappingTable.capture(encoded, paths));
  EXPECT_EQ(state.files, originals);
  paths[2].backup = "/BACKUP/ITEMS";
  EXPECT_FALSE(capture.capture(encoded, paths));
  EXPECT_EQ(state.files, originals);
  paths[2].backup = "/backup/profile";
  state.files["/profile"][0] ^= 1;
  EXPECT_FALSE(capture.capture(encoded, paths));
  EXPECT_FALSE(state.files.contains("/backup/manifest"));
  EXPECT_EQ(state.files["/backup/intent"], std::vector<uint8_t>(encoded.begin(), encoded.end()));
  auto conflicting = encoded;
  conflicting[52] ^= 1;
  tinta_body_detail::write(conflicting, 432, tinta::core::crc32(conflicting.data(), 432), 4);
  const auto interrupted = state.files;
  EXPECT_FALSE(capture.capture(conflicting, paths));
  EXPECT_EQ(state.files, interrupted);
  EXPECT_EQ(state.files["/reviews"], originals.at("/reviews"));
  EXPECT_EQ(state.files["/items"], originals.at("/items"));
  {
    HalLegacyTintaBackupCapture restarted("/backup", "/backup/manifest", "/backup/manifest-next", scratch, nullptr,
                                          nullptr, "/backup/intent", "/backup/intent-next");
    Identity wrongGeneration = manifest.generation;
    wrongGeneration[0] ^= 1;
    EXPECT_FALSE(restarted.recover(manifest.reader, wrongGeneration, manifest.course, manifest.transaction, paths));
    EXPECT_EQ(state.files, interrupted);
    EXPECT_FALSE(restarted.recover(manifest.reader, manifest.generation, manifest.course, manifest.transaction, paths));
    EXPECT_EQ(state.files, interrupted);
    state.files["/profile"] = originals.at("/profile");
    ASSERT_TRUE(restarted.recover(manifest.reader, manifest.generation, manifest.course, manifest.transaction, paths));
    const auto recovered = state.files;
    ASSERT_TRUE(restarted.recover(manifest.reader, manifest.generation, manifest.course, manifest.transaction, paths));
    EXPECT_EQ(state.files, recovered);
  }
  EXPECT_EQ(state.files["/backup/manifest"], std::vector<uint8_t>(encoded.begin(), encoded.end()));
  const auto completed = state.files;
  std::array<const char*, LEGACY_TINTA_BACKUP_ROLES> backups{};
  for (size_t at = 0; at < 3; ++at) backups[at] = paths[at].backup;
  ASSERT_TRUE(
      capture.verifySaved(manifest.reader, manifest.generation, manifest.course, manifest.transaction, backups));
  Identity foreign = manifest.generation;
  foreign[0] ^= 1;
  EXPECT_FALSE(capture.verifySaved(manifest.reader, foreign, manifest.course, manifest.transaction, backups));
  EXPECT_EQ(state.files, completed);
  backups[2] = "/BACKUP/ITEMS";
  EXPECT_FALSE(
      capture.verifySaved(manifest.reader, manifest.generation, manifest.course, manifest.transaction, backups));
  EXPECT_EQ(state.files, completed);
  backups[2] = paths[2].backup;
  Identity invalid{};
  EXPECT_FALSE(capture.captureSources(invalid, manifest.generation, manifest.course, manifest.transaction, paths));
  EXPECT_EQ(state.files, completed);
  state.files.erase("/profile");
  const auto missingSource = state.files;
  ASSERT_TRUE(
      capture.verifySaved(manifest.reader, manifest.generation, manifest.course, manifest.transaction, backups));
  EXPECT_EQ(state.files, missingSource);
  EXPECT_FALSE(
      capture.captureSources(manifest.reader, manifest.generation, manifest.course, manifest.transaction, paths));
  EXPECT_EQ(state.files, missingSource);
  state.files["/profile"] = originals.at("/profile");
  ASSERT_TRUE(capture.capture(encoded, paths));
  EXPECT_EQ(state.files, completed);
  state.files["/backup/items"][0] ^= 1;
  const auto corrupted = state.files;
  EXPECT_FALSE(
      capture.verifySaved(manifest.reader, manifest.generation, manifest.course, manifest.transaction, backups));
  EXPECT_EQ(state.files, corrupted);
  EXPECT_FALSE(capture.capture(encoded, paths));
  EXPECT_EQ(state.files, corrupted);
}

TEST(HalTintaJournalStorageTest, LegacyBackupResumedCandidateSyncFailurePreservesFiles) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  static constexpr char ROOT[] = "/backup";
  static constexpr char SOURCE[] = "/source.log";
  static constexpr char STAGE[] = "/backup/next";
  static constexpr char BACKUP[] = "/backup/reviews.log";
  ASSERT_TRUE(Storage.ensureDirectoryExists(ROOT));
  state.files[SOURCE] = std::vector<uint8_t>(400, 7);
  state.files[STAGE] = state.files[SOURCE];
  const auto original = state.files;
  Digest hash{};
  SHA256(state.files[SOURCE].data(), state.files[SOURCE].size(), hash.data());
  std::array<uint8_t, 128> scratch{};
  HalLegacyTintaBackupCopy copier(ROOT, scratch);
  state.failSync = true;
  EXPECT_FALSE(copier.copy(SOURCE, STAGE, BACKUP, 400, hash));
  EXPECT_EQ(state.files, original);
  state.failSync = false;
  ASSERT_TRUE(copier.copy(SOURCE, STAGE, BACKUP, 400, hash));
  EXPECT_EQ(state.files[BACKUP], original.at(SOURCE));
  EXPECT_EQ(state.files[SOURCE], original.at(SOURCE));
  EXPECT_FALSE(state.files.contains(STAGE));
}

TEST(HalTintaJournalStorageTest, LegacyBackupWriteAndSealFailuresCleanOwnedStageAndPermitRetry) {
  auto& state = inventory_hal_test::state;
  for (const bool syncFailure : {false, true}) {
    state = {};
    state.enumerateFileMap = true;
    static constexpr char ROOT[] = "/backup";
    static constexpr char SOURCE[] = "/source.log";
    static constexpr char STAGE[] = "/backup/next";
    static constexpr char BACKUP[] = "/backup/reviews.log";
    ASSERT_TRUE(Storage.ensureDirectoryExists(ROOT));
    state.files[SOURCE] = std::vector<uint8_t>(400, 7);
    const auto original = state.files[SOURCE];
    Digest hash{};
    SHA256(original.data(), original.size(), hash.data());
    std::array<uint8_t, 128> scratch{};
    HalLegacyTintaBackupCopy copier(ROOT, scratch);
    state.failWrite = !syncFailure;
    state.failSync = syncFailure;
    EXPECT_FALSE(copier.copy(SOURCE, STAGE, BACKUP, original.size(), hash));
    EXPECT_EQ(state.files[SOURCE], original);
    EXPECT_FALSE(state.files.contains(STAGE));
    EXPECT_FALSE(state.files.contains(BACKUP));
    state.failWrite = state.failSync = false;
    ASSERT_TRUE(copier.copy(SOURCE, STAGE, BACKUP, original.size(), hash));
    EXPECT_EQ(state.files[BACKUP], original);
    EXPECT_EQ(state.files[SOURCE], original);
  }
}

TEST(HalTintaJournalStorageTest, LegacyManifestPublicationResumesSealedStageAndPreservesConflicts) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  static constexpr char ROOT[] = "/legacy-backup";
  static constexpr char PATH[] = "/legacy-backup/manifest";
  static constexpr char STAGE[] = "/legacy-backup/manifest-next";
  ASSERT_TRUE(Storage.ensureDirectoryExists(ROOT));
  std::string fixture = TINTA_FRONTIER_FIXTURE;
  fixture.resize(fixture.find_last_of('/') + 1);
  std::ifstream input(fixture + "LegacyReaderBackupManifest-v1.fixture", std::ios::binary);
  ASSERT_TRUE(input.good());
  const std::vector<uint8_t> expected{std::istreambuf_iterator<char>(input), {}};
  HalLegacyTintaBackupManifestStore store(ROOT, PATH, STAGE);
  std::array<uint8_t, LEGACY_TINTA_BACKUP_MANIFEST_SIZE> loaded{};
  loaded.fill(0x55);
  const auto unchanged = loaded;
  EXPECT_EQ(store.load(loaded), CompanionFilePresence::Missing);
  EXPECT_EQ(loaded, unchanged);
  EXPECT_EQ(store.load(std::span<uint8_t>(loaded).first(100)), CompanionFilePresence::Error);
  EXPECT_EQ(loaded, unchanged);
  state.files[PATH] = expected;
  state.files[PATH][0] ^= 1;
  const auto corruptedFiles = state.files;
  EXPECT_EQ(store.load(loaded), CompanionFilePresence::Error);
  EXPECT_EQ(loaded, unchanged);
  EXPECT_EQ(state.files, corruptedFiles);
  state.files[PATH] = {1, 2, 3};
  EXPECT_EQ(store.load(loaded), CompanionFilePresence::Error);
  EXPECT_EQ(loaded, unchanged);
  state.files.erase(PATH);
  state.files[STAGE] = {1, 2, 3};
  const auto unknown = state.files;
  EXPECT_FALSE(store.persist(expected));
  EXPECT_EQ(state.files, unknown);
  state.files.erase(STAGE);
  state.failRename = state.renames + 1;
  EXPECT_FALSE(store.persist(expected));
  EXPECT_EQ(state.files[STAGE], expected);
  EXPECT_FALSE(state.files.contains(PATH));
  state.failRename = 0;
  ASSERT_TRUE(store.persist(expected));
  EXPECT_EQ(state.files[PATH], expected);
  ASSERT_EQ(store.load(loaded), CompanionFilePresence::Present);
  EXPECT_TRUE(std::equal(loaded.begin(), loaded.end(), expected.begin(), expected.end()));
  const auto published = state.files;
  ASSERT_TRUE(store.persist(expected));
  EXPECT_EQ(state.files, published);
  auto changed = expected;
  changed[52] ^= 1;
  tinta_body_detail::write(changed, 432, tinta::core::crc32(changed.data(), 432), 4);
  EXPECT_FALSE(store.persist(changed));
  EXPECT_EQ(state.files, published);
  state.files.erase(PATH);
  state.failRenameAfterSource = STAGE;
  EXPECT_FALSE(store.persist(expected));
  EXPECT_EQ(state.files[PATH], expected);
  state.failRenameAfterSource.clear();
  ASSERT_TRUE(store.persist(expected));
  EXPECT_EQ(state.files, published);
}

TEST(HalTintaJournalStorageTest, AbortBeforeBeginPersistsReceiptAndRejectsLateBeginAfterRestart) {
  auto& state = inventory_hal_test::state;
  for (unsigned failure = 0; failure < 3; ++failure) {
    state = {};
    state.enumerateFileMap = true;
    {
      HalTintaJournalStorage storage;
      std::array<uint8_t, 1024> scratch{};
      TintaJournal journal(storage, scratch);
      ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
    }
    auto expected = mergeIntent();
    {
      HalJournalCausalAuditSession audit;
      ASSERT_TRUE(audit.run(&expected.previous.frontier));
      expected.previous.count = audit.recordCount();
      expected.previous.recordSize = audit.recordSize();
    }
    const auto original = state.files.at(TINTA_JOURNAL_EVENTS);
    {
      HalJournalMergeCandidateSession session;
      if (failure != 0) {
        state.failWrite = failure == 1;
        state.failSync = failure == 2;
        EXPECT_EQ(session.abort(expected, expected.generation), TintaJournalResult::IoError);
        state.failWrite = state.failSync = false;
        EXPECT_EQ(state.files.at(TINTA_JOURNAL_EVENTS), original);
      }
      ASSERT_EQ(session.abort(expected, expected.generation), TintaJournalResult::Ok);
      EXPECT_FALSE(session.available());
      EXPECT_FALSE(state.files.contains(HalJournalMergeRecordStore::RECEIVING));
      EXPECT_FALSE(state.directories.contains(MERGE_TINTA_JOURNAL_PATHS.directory));
    }
    HalJournalMergeCandidateSession restarted;
    EXPECT_EQ(restarted.begin(expected, expected.generation), TintaJournalResult::Conflict);
    const auto cancelledFiles = state.files;
    EXPECT_EQ(restarted.abort(expected, expected.generation), TintaJournalResult::Ok);
    EXPECT_EQ(state.files, cancelledFiles);
    auto foreign = expected;
    foreign.owner[0] ^= 1;
    EXPECT_EQ(restarted.abort(foreign, foreign.generation), TintaJournalResult::Conflict);
    EXPECT_EQ(state.files, cancelledFiles);
    auto next = expected;
    next.transaction[0] ^= 1;
    ASSERT_EQ(restarted.begin(next, next.generation), TintaJournalResult::Ok);
    ASSERT_EQ(restarted.abort(next, next.generation), TintaJournalResult::Ok);
    EXPECT_EQ(restarted.begin(expected, expected.generation), TintaJournalResult::Conflict);
    EXPECT_EQ(state.files.at(TINTA_JOURNAL_EVENTS), original);
  }
}

TEST(HalTintaJournalStorageTest, CandidateCommitRefusesUnresolvedPreferencesBeforePublication) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
  std::array<uint8_t, 8> first{1, 4, 32, 1, 10, 0, 0, 0};
  auto second = first;
  second[4] = 20;
  SyncEvent event;
  event.identity.epoch = event.identity.sequence = 1;
  event.storageGeneration.fill(2);
  event.kind = EventKind::Preference;
  event.resource = PREFERENCE_SCOPE;
  for (const auto location : {TintaJournalLocation::Active, TintaJournalLocation::MergeCandidate}) {
    HalTintaJournalStorage storage(location);
    std::array<uint8_t, 1024> scratch{};
    TintaJournal journal(storage, scratch);
    ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
    event.identity.origin.fill(1);
    ASSERT_TRUE(storage.digest(first, event.bodyHash));
    ASSERT_EQ(journal.append(event, first), TintaJournalResult::Ok);
    if (location == TintaJournalLocation::MergeCandidate) {
      event.identity.origin.fill(3);
      ASSERT_TRUE(storage.digest(second, event.bodyHash));
      ASSERT_EQ(journal.append(event, second), TintaJournalResult::Ok);
    }
  }
  auto expected = mergeIntent();
  {
    HalJournalCausalAuditSession active, candidate(TintaJournalLocation::MergeCandidate);
    ASSERT_TRUE(active.run(&expected.previous.frontier));
    expected.previous.count = active.recordCount();
    expected.previous.recordSize = active.recordSize();
    ASSERT_TRUE(candidate.run(&expected.merged.frontier));
    expected.merged.count = candidate.recordCount();
    expected.merged.recordSize = candidate.recordSize();
  }
  const auto original = state.files.at(TINTA_JOURNAL_EVENTS);
  for (const auto* path :
       {MERGE_TINTA_JOURNAL_PATHS.events, MERGE_TINTA_JOURNAL_PATHS.headerA, MERGE_TINTA_JOURNAL_PATHS.headerB})
    state.files.erase(path);
  state.directories.erase(MERGE_TINTA_JOURNAL_PATHS.directory);
  HalJournalMergeCandidateSession session;
  ASSERT_EQ(session.begin(expected, expected.generation), TintaJournalResult::Ok);
  ASSERT_TRUE(session.available());
  ASSERT_EQ(session.append(event, second), TintaJournalResult::Ok);
  EXPECT_EQ(session.commit(expected.generation, nullptr, nullptr), TintaJournalResult::Conflict);
  EXPECT_FALSE(session.available());
  EXPECT_EQ(state.files.at(TINTA_JOURNAL_EVENTS), original);
  EXPECT_TRUE(state.files.contains(MERGE_TINTA_JOURNAL_PATHS.events));
  ASSERT_EQ(session.abort(expected, expected.generation), TintaJournalResult::Ok);
  EXPECT_FALSE(session.available());
  EXPECT_EQ(state.files.at(TINTA_JOURNAL_EVENTS), original);

  HalJournalMergeReceiveSession receiver;
  expected.transaction[0] ^= 1;
  std::array<uint8_t, JOURNAL_MERGE_INTENT_SIZE> declaration{};
  ASSERT_TRUE(encodeJournalMergeIntent(expected, declaration));
  std::array<uint8_t, MAX_CONTROL_PAYLOAD> payload{};
  JournalMergeRequestView request;
  request.transaction = expected.transaction;
  request.declaration = declaration;
  auto dispatch = [&]() {
    const auto size = encodeJournalMergeRequest(request, payload);
    EXPECT_NE(size, 0u);
    return receiver.dispatch(std::span(payload).first(size), expected.owner, expected.generation, nullptr, nullptr);
  };
  ASSERT_EQ(dispatch(), TintaJournalResult::Ok);
  ASSERT_NE(receiver.receivingDeclaration(), nullptr);
  std::array<uint8_t, MAX_RECORD_SIZE> envelope{};
  const auto envelopeSize = encodeRecord(event, envelope);
  ASSERT_NE(envelopeSize, 0u);
  request.operation = JournalMergeOperation::Append;
  request.declaration = {};
  request.envelope = std::span(envelope).first(envelopeSize);
  request.body = second;
  ASSERT_EQ(dispatch(), TintaJournalResult::Ok);
  request.operation = JournalMergeOperation::Commit;
  request.declaration = declaration;
  request.envelope = request.body = {};
  EXPECT_EQ(dispatch(), TintaJournalResult::Conflict);
  EXPECT_TRUE(receiver.hasBinding());
  EXPECT_EQ(receiver.receivingDeclaration(), nullptr);
  EXPECT_EQ(state.files.at(TINTA_JOURNAL_EVENTS), original);
  request.operation = JournalMergeOperation::Begin;
  ASSERT_EQ(dispatch(), TintaJournalResult::Ok);
  EXPECT_NE(receiver.receivingDeclaration(), nullptr);
  request.operation = JournalMergeOperation::Abort;
  ASSERT_EQ(dispatch(), TintaJournalResult::Ok);
  EXPECT_FALSE(receiver.hasBinding());
  EXPECT_EQ(receiver.receivingDeclaration(), nullptr);
}

TEST(HalTintaJournalStorageTest, BootJournalRecoverySkipsIdentityWhenAbsentAndPreservesFailedIntents) {
  class Identities final : public IdentityStorage {
   public:
    unsigned calls = 0;
    bool hardwareIdentity(Identity&) override {
      ++calls;
      return false;
    }
    bool cardIdentity(Identity&) override {
      ++calls;
      return false;
    }
    IdentityRead readBinding(std::span<uint8_t>) override {
      ++calls;
      return IdentityRead::Error;
    }
    bool writeBinding(std::span<const uint8_t>) override {
      ++calls;
      return false;
    }
    IdentityRead readMarker(Identity&) override {
      ++calls;
      return IdentityRead::Error;
    }
    bool createMarker(const Identity&) override {
      ++calls;
      return false;
    }
    bool randomIdentity(Identity&) override {
      ++calls;
      return false;
    }
  } identities;
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  HalJournalStartupRecovery recovery;
  ASSERT_TRUE(recovery.run(identities));
  EXPECT_EQ(identities.calls, 0U);
  EXPECT_TRUE(state.files.empty());
  state.files[HalJournalMergeRecordStore::ABORTING] = {1, 2, 3};
  const auto corrupt = state.files;
  EXPECT_FALSE(recovery.run(identities));
  EXPECT_EQ(identities.calls, 0U);
  EXPECT_EQ(state.files, corrupt);
  state.files.clear();
  JournalMergeIntent pending;
  pending.owner.fill(1);
  pending.transaction.fill(2);
  pending.generation.fill(3);
  pending.previous.recordSize = 512;
  pending.merged.recordSize = 1024;
  pending.previous.frontier.fill(4);
  pending.merged.frontier = pending.previous.frontier;
  HalJournalMergeRecordStore aborting(JournalMergeRecord::Aborting);
  ASSERT_TRUE(aborting.persist(pending));
  const auto saved = state.files;
  EXPECT_FALSE(recovery.run(identities));
  EXPECT_EQ(identities.calls, 1U);
  EXPECT_EQ(state.files, saved);
}

TEST(HalTintaJournalStorageTest, CandidateSessionResumesAppendAndCommitWithoutChangingActiveEarly) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
  TintaBody body;
  body.kind = EventKind::Star;
  body.course.fill(7);
  body.uid = 1;
  Identity course = body.course;
  class Catalog final : public TintaSubjectCatalog {
   public:
    TintaSubjectMembership result = TintaSubjectMembership::Present;
    unsigned calls = 0;
    TintaSubjectMembership contains(EventKind kind, uint32_t uid) override {
      ++calls;
      EXPECT_EQ(kind, EventKind::Star);
      EXPECT_EQ(uid, 1u);
      return result;
    }
  } catalog;
  std::array<uint8_t, MAX_TINTA_BODY_SIZE> bytes{};
  const auto length = encodeTintaBody(body, bytes);
  SyncEvent event;
  event.identity.origin.fill(1);
  event.identity.epoch = 1;
  event.storageGeneration.fill(2);
  event.resource.fill(3);
  event.kind = EventKind::Star;
  for (const auto location : {TintaJournalLocation::Active, TintaJournalLocation::MergeCandidate}) {
    HalTintaJournalStorage storage(location);
    std::array<uint8_t, 1024> scratch{};
    TintaJournal journal(storage, scratch);
    ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
    ASSERT_TRUE(storage.digest(std::span(bytes).first(length), event.bodyHash));
    for (uint64_t sequence = 1; sequence <= (location == TintaJournalLocation::Active ? 1u : 3u); ++sequence) {
      event.identity.sequence = sequence;
      ASSERT_EQ(journal.append(event, std::span(bytes).first(length)), TintaJournalResult::Ok);
    }
  }
  JournalMergeIntent expected = mergeIntent();
  {
    HalJournalCausalAuditSession active, candidate(TintaJournalLocation::MergeCandidate);
    ASSERT_TRUE(active.run(&expected.previous.frontier));
    expected.previous.count = active.recordCount();
    expected.previous.recordSize = active.recordSize();
    ASSERT_TRUE(candidate.run(&expected.merged.frontier));
    expected.merged.count = candidate.recordCount();
    expected.merged.recordSize = candidate.recordSize();
  }
  const auto oldEvents = state.files[TINTA_JOURNAL_EVENTS];
  const auto mergedEvents = state.files[MERGE_TINTA_JOURNAL_PATHS.events];
  for (const auto* path :
       {MERGE_TINTA_JOURNAL_PATHS.events, MERGE_TINTA_JOURNAL_PATHS.headerA, MERGE_TINTA_JOURNAL_PATHS.headerB})
    state.files.erase(path);
  state.directories.erase(MERGE_TINTA_JOURNAL_PATHS.directory);
  {
    HalJournalMergeCandidateSession session;
    ASSERT_EQ(session.begin(expected, expected.generation), TintaJournalResult::Ok);
    EXPECT_EQ(session.count(), 1u);
    event.identity.sequence = 2;
    ASSERT_EQ(session.append(event, std::span(bytes).first(length)), TintaJournalResult::Ok);
    EXPECT_EQ(session.commit(expected.generation, &course, &catalog), TintaJournalResult::Conflict);
    EXPECT_EQ(state.files[TINTA_JOURNAL_EVENTS], oldEvents);
    ASSERT_TRUE(session.close());
  }
  const auto beforeAbort = state;
  {
    HalJournalMergeCandidateSession session;
    auto foreign = expected;
    foreign.owner[0] ^= 1;
    EXPECT_EQ(session.abort(foreign, foreign.generation), TintaJournalResult::Conflict);
    EXPECT_FALSE(state.files.contains(HalJournalMergeRecordStore::ABORTING));
    state.failRemoveAfter = true;
    EXPECT_EQ(session.abort(expected, expected.generation), TintaJournalResult::IoError);
    state.failRemoveAfter = false;
    EXPECT_TRUE(state.files.contains(HalJournalMergeRecordStore::ABORTING));
    EXPECT_TRUE(state.files.contains(HalJournalMergeRecordStore::RECEIVING));
    EXPECT_EQ(session.begin(expected, expected.generation), TintaJournalResult::Conflict);
    const std::string unexpected = std::string(MERGE_TINTA_JOURNAL_PATHS.directory) + "/unexpected.bin";
    state.files[unexpected] = {9};
    EXPECT_EQ(session.abort(expected, expected.generation), TintaJournalResult::IoError);
    EXPECT_EQ(state.files.at(unexpected), (std::vector<uint8_t>{9}));
    EXPECT_TRUE(state.files.contains(HalJournalMergeRecordStore::ABORTING));
    state.files.erase(unexpected);
    state.failRenameAfterSource = "/.crosspoint/companion/journal-merge-aborted-02020202020202020202020202020202-next";
    EXPECT_EQ(session.abort(expected, expected.generation), TintaJournalResult::IoError);
    state.failRenameAfterSource.clear();
    EXPECT_TRUE(state.files.contains(HalJournalMergeRecordStore::ABORTING));
    auto changedGeneration = expected.generation;
    changedGeneration[0] ^= 1;
    const auto interruptedAbort = state.files;
    EXPECT_FALSE(recoverExistingJournalMergeAbort(changedGeneration));
    EXPECT_EQ(state.files, interruptedAbort);
    EXPECT_TRUE(recoverExistingJournalMergeAbort(expected.generation));
    const auto recoveredAbort = state.files;
    EXPECT_TRUE(recoverExistingJournalMergeAbort(expected.generation));
    EXPECT_EQ(state.files, recoveredAbort);
    EXPECT_EQ(session.abort(expected, expected.generation), TintaJournalResult::Ok);
    EXPECT_FALSE(state.files.contains(HalJournalMergeRecordStore::ABORTING));
    EXPECT_FALSE(state.files.contains(HalJournalMergeRecordStore::RECEIVING));
    EXPECT_FALSE(state.directories.contains(MERGE_TINTA_JOURNAL_PATHS.directory));
    EXPECT_EQ(state.files[TINTA_JOURNAL_EVENTS], oldEvents);
    EXPECT_EQ(session.begin(expected, expected.generation), TintaJournalResult::Conflict);
    auto next = expected;
    next.transaction[0] ^= 1;
    ASSERT_EQ(session.begin(next, next.generation), TintaJournalResult::Ok);
    ASSERT_EQ(session.abort(next, next.generation), TintaJournalResult::Ok);
    EXPECT_EQ(session.begin(expected, expected.generation), TintaJournalResult::Conflict);
  }
  state = beforeAbort;
  {
    HalJournalMergeCandidateSession session;
    auto foreign = expected;
    foreign.owner[0] ^= 1;
    EXPECT_EQ(session.begin(foreign, foreign.generation), TintaJournalResult::Conflict);
    ASSERT_EQ(session.begin(expected, expected.generation), TintaJournalResult::Ok);
    EXPECT_EQ(session.count(), 2u);
    EXPECT_EQ(session.append(event, std::span(bytes).first(length)), TintaJournalResult::Duplicate);
    event.identity.sequence = 3;
    ASSERT_EQ(session.append(event, std::span(bytes).first(length)), TintaJournalResult::Ok);
    event.identity.sequence = 4;
    EXPECT_EQ(session.append(event, std::span(bytes).first(length)), TintaJournalResult::Exhausted);
    EXPECT_EQ(session.count(), 3u);
    const auto activeBeforeValidation = state.files[TINTA_JOURNAL_EVENTS];
    EXPECT_EQ(session.commit(expected.generation, nullptr, nullptr), TintaJournalResult::Invalid);
    Identity otherCourse{};
    otherCourse.fill(8);
    EXPECT_EQ(session.commit(expected.generation, &otherCourse, &catalog), TintaJournalResult::Invalid);
    catalog.result = TintaSubjectMembership::Missing;
    EXPECT_EQ(session.commit(expected.generation, &course, &catalog), TintaJournalResult::Invalid);
    catalog.result = TintaSubjectMembership::IoError;
    EXPECT_EQ(session.commit(expected.generation, &course, &catalog), TintaJournalResult::IoError);
    EXPECT_EQ(state.files[TINTA_JOURNAL_EVENTS], activeBeforeValidation);
    EXPECT_FALSE(state.files.contains(HalJournalMergeRecordStore::INTENT));
    catalog.result = TintaSubjectMembership::Present;
    catalog.calls = 0;
    state.failRenameAfterSource = TINTA_JOURNAL_DIRECTORY;
    EXPECT_EQ(session.commit(expected.generation, &course, &catalog), TintaJournalResult::IoError);
    EXPECT_EQ(catalog.calls, 2u);
    EXPECT_TRUE(state.files.contains(HalJournalMergeRecordStore::INTENT));
    EXPECT_EQ(session.abort(expected, expected.generation), TintaJournalResult::Conflict);
  }
  state.failRenameAfterSource.clear();
  {
    HalJournalMergeCandidateSession session;
    EXPECT_EQ(session.begin(expected, expected.generation), TintaJournalResult::Duplicate);
    EXPECT_EQ(session.abort(expected, expected.generation), TintaJournalResult::Conflict);
    EXPECT_EQ(session.begin(expected, expected.generation), TintaJournalResult::Duplicate);
  }
  EXPECT_EQ(state.files[TINTA_JOURNAL_EVENTS], mergedEvents);
  EXPECT_EQ(state.files[MERGE_BACKUP_TINTA_JOURNAL_PATHS.events], oldEvents);
  EXPECT_FALSE(state.files.contains(HalJournalMergeRecordStore::INTENT));
  EXPECT_FALSE(state.files.contains(HalJournalMergeRecordStore::RECEIVING));
  EXPECT_TRUE(state.files.contains(HalJournalMergeRecordStore::RECEIPT));
}

TEST(HalTintaJournalStorageTest, ReusedRecordReadsYieldPeriodicallyAndCloseResetsBudget) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.files["/.crosspoint/companion/tinta-events/events.bin"] = {1, 2, 3};
  HalTintaJournalStorage storage;
  std::array<uint8_t, 3> bytes{};
  for (unsigned at = 0; at < 31; ++at) ASSERT_TRUE(storage.read(0, bytes));
  EXPECT_EQ(state.yields, 0u);
  ASSERT_TRUE(storage.read(0, bytes));
  EXPECT_EQ(state.yields, 1u);
  const auto preparations = state.preparations;
  for (unsigned at = 0; at < 32; ++at) ASSERT_TRUE(storage.read(0, bytes));
  EXPECT_EQ(state.yields, 2u);
  EXPECT_EQ(state.preparations, preparations);
  for (unsigned at = 0; at < 16; ++at) ASSERT_TRUE(storage.read(0, bytes));
  storage.close();
  for (unsigned at = 0; at < 16; ++at) ASSERT_TRUE(storage.read(0, bytes));
  EXPECT_EQ(state.yields, 2u);
  for (unsigned at = 0; at < 16; ++at) ASSERT_TRUE(storage.read(0, bytes));
  EXPECT_EQ(state.yields, 3u);
  EXPECT_EQ(bytes, (std::array<uint8_t, 3>{1, 2, 3}));
}

TEST(HalTintaJournalStorageTest, EnumerationFailureCannotInitializeOrReplaceHeaders) {
  auto& state = inventory_hal_test::state;
  state = {};
  static constexpr char DIRECTORY[] = "/.crosspoint/companion/tinta-events";
  state.directories[DIRECTORY] = {};
  state.enumerateFileMap = true;
  HalTintaJournalStorage storage;
  std::array<uint8_t, 512> scratch{};
  TintaJournal journal(storage, scratch);
  state.directoryErrorPath = DIRECTORY;
  EXPECT_EQ(journal.open(), TintaJournalResult::IoError);
  EXPECT_EQ(state.files.size(), 1u);
  state.directoryErrorPath.clear();
  ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
  TintaBody body;
  body.kind = EventKind::Star;
  body.course.fill(7);
  body.uid = 1;
  body.enabled = true;
  std::array<uint8_t, MAX_TINTA_BODY_SIZE> encoded{};
  const auto bodyLength = encodeTintaBody(body, encoded);
  ASSERT_NE(bodyLength, 0u);
  SyncEvent event;
  event.identity.origin.fill(1);
  event.identity.epoch = event.identity.sequence = 1;
  event.storageGeneration.fill(2);
  event.resource.fill(3);
  event.kind = body.kind;
  ASSERT_TRUE(storage.digest(std::span(encoded).first(bodyLength), event.bodyHash));
  ASSERT_EQ(journal.append(event, std::span(encoded).first(bodyLength)), TintaJournalResult::Ok);
  const auto saved = state.files;
  state.falseExists = true;
  ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
  EXPECT_EQ(state.files, saved);
  state.directoryErrorPath = DIRECTORY;
  EXPECT_EQ(journal.open(), TintaJournalResult::IoError);
  EXPECT_EQ(state.files, saved);
  state.directoryErrorPath.clear();
  ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
  EXPECT_EQ(journal.count(), 1u);
  ASSERT_EQ(journal.read(0), TintaJournalResult::Ok);
  EXPECT_EQ(journal.event(), event);
  EXPECT_EQ(state.files, saved);
}

TEST(HalTintaJournalStorageTest, HeaderReadsUseRealProviderAndPreserveMalformedBytes) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.directories["/.crosspoint/companion/tinta-events"] = {};
  state.enumerateFileMap = true;
  static constexpr char HEADER[] = "/.crosspoint/companion/tinta-events/header-a.bin";
  state.files[HEADER] = {1, 2, 3};
  HalTintaJournalStorage storage;
  std::array<uint8_t, TintaJournal::HEADER_SIZE> bytes{};
  size_t length = 0;
  ASSERT_TRUE(storage.readHeader(0, bytes, length));
  EXPECT_EQ(length, bytes.size() + 1);
  EXPECT_EQ(state.files.at(HEADER), std::vector<uint8_t>({1, 2, 3}));
  std::array<uint8_t, 512> scratch{};
  TintaJournal journal(storage, scratch);
  EXPECT_EQ(journal.open(), TintaJournalResult::Corrupt);
  EXPECT_EQ(state.files.at(HEADER), std::vector<uint8_t>({1, 2, 3}));
}

TEST(HalTintaJournalStorageTest, StartupAuditSkipsAbsentJournalAndAuditsExistingHistoryBeforeUse) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  HalCompanionFileLookup lookup;
  ASSERT_TRUE(auditExistingCompanionJournal(lookup));
  EXPECT_TRUE(state.files.empty());
  state.directories[TINTA_JOURNAL_DIRECTORY] = {};
  SyncEvent event;
  {
    HalTintaJournalStorage storage;
    std::array<uint8_t, 512> scratch{};
    TintaJournal journal(storage, scratch);
    ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
    TintaBody body;
    body.kind = EventKind::Star;
    body.course.fill(7);
    body.uid = 1;
    body.enabled = true;
    std::array<uint8_t, MAX_TINTA_BODY_SIZE> bytes{};
    const auto length = encodeTintaBody(body, bytes);
    event.identity.origin.fill(1);
    event.identity.epoch = event.identity.sequence = 1;
    event.storageGeneration.fill(2);
    event.resource.fill(3);
    event.kind = EventKind::Star;
    ASSERT_TRUE(storage.digest(std::span(bytes).first(length), event.bodyHash));
    ASSERT_EQ(journal.append(event, std::span(bytes).first(length)), TintaJournalResult::Ok);
    event.identity.sequence = 2;
    ASSERT_EQ(journal.append(event, std::span(bytes).first(length)), TintaJournalResult::Ok);
  }
  const auto original = state.files;
  HalJournalCausalAuditSession replay;
  std::vector<uint64_t> sequences;
  const auto collect = [](void* context, uint32_t, const SyncEvent& event, std::span<const uint8_t> body, bool) {
    TintaBody decoded;
    if (!decodeTintaBody(body, decoded)) return false;
    static_cast<std::vector<uint64_t>*>(context)->push_back(event.identity.sequence);
    return true;
  };
  EXPECT_FALSE(replay.replay(&sequences, collect));
  ASSERT_TRUE(replay.run());
  Identity invalidCourse{};
  EXPECT_FALSE(replay.run(nullptr, &invalidCourse));
  EXPECT_FALSE(replay.replay(&sequences, collect));
  EXPECT_TRUE(sequences.empty());
  ASSERT_TRUE(replay.run());
  ASSERT_TRUE(replay.replay(&sequences, collect));
  EXPECT_EQ(sequences, (std::vector<uint64_t>{1, 2}));
  EXPECT_FALSE(replay.replay(&sequences, collect));
  ASSERT_TRUE(replay.run());
  EXPECT_FALSE(
      replay.replay(nullptr, [](void*, uint32_t, const SyncEvent&, std::span<const uint8_t>, bool) { return false; }));
  ASSERT_TRUE(auditExistingCompanionJournal(lookup));
  for (const auto* path : {TINTA_JOURNAL_EVENTS, TINTA_JOURNAL_HEADER_A, TINTA_JOURNAL_HEADER_B})
    EXPECT_EQ(state.files[path], original.at(path));
  ASSERT_TRUE(replay.beginExport());
  std::array<uint8_t, JOURNAL_EXPORT_REQUEST_SIZE> cursor{};
  std::array<uint8_t, MAX_CONTROL_PAYLOAD> page{};
  ASSERT_GT(replay.exportPage(cursor, page), JOURNAL_EXPORT_HEADER_SIZE);
  EXPECT_EQ(tinta_body_detail::read(page, 4, 4), 2u);
  std::copy_n(page.begin() + 12, 32, cursor.begin());
  tinta_body_detail::write(cursor, 32, 2, 4);
  tinta_body_detail::write(cursor, 36, 1, 4);
  ASSERT_GT(replay.exportPage(cursor, page), JOURNAL_EXPORT_HEADER_SIZE);
  SyncEvent delivered;
  ASSERT_TRUE(decodeRecord(std::span(page).subspan(48, tinta_body_detail::read(page, 44, 2)), delivered));
  EXPECT_EQ(delivered.identity.sequence, 2u);
  tinta_body_detail::write(cursor, 36, 2, 4);
  EXPECT_EQ(replay.exportPage(cursor, page), JOURNAL_EXPORT_HEADER_SIZE);
  EXPECT_EQ(page[1], 1);
  ASSERT_TRUE(replay.endExport());
  EXPECT_EQ(replay.exportPage(cursor, page), 0u);
  EXPECT_TRUE(state.files.contains(HalJournalIdentityIndexStorage::PATH));
  state.failSync = true;
  EXPECT_FALSE(auditExistingCompanionJournal(lookup));
  state.failSync = false;
  ASSERT_TRUE(auditExistingCompanionJournal(lookup));
  event.identity.sequence = 3;
  auto record = std::span(state.files[TINTA_JOURNAL_EVENTS]).subspan(512, 512);
  ASSERT_NE(encodeRecord(event, record.subspan(12, MAX_RECORD_SIZE)), 0u);
  tinta_body_detail::write(record, 508, tinta::core::crc32(record.data(), 508), 4);
  const auto orphaned = state.files[TINTA_JOURNAL_EVENTS];
  EXPECT_FALSE(auditExistingCompanionJournal(lookup));
  EXPECT_EQ(state.files[TINTA_JOURNAL_EVENTS], orphaned);
}

TEST(HalTintaJournalStorageTest, AuditedHalFrontierMatchesSharedAppleFixtureAndPreservesOutputOnFailure) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  state.directories[TINTA_JOURNAL_DIRECTORY] = {};
  std::ifstream input(TINTA_FRONTIER_FIXTURE, std::ios::binary);
  ASSERT_TRUE(input.good());
  const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
  ASSERT_EQ(bytes.size(), 410u);
  {
    HalTintaJournalStorage storage;
    std::array<uint8_t, 512> scratch{};
    TintaJournal journal(storage, scratch);
    ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
    size_t at = 12;
    for (unsigned eventIndex = 0; eventIndex < 2; ++eventIndex) {
      const auto length = bytes[at] | (static_cast<size_t>(bytes[at + 1]) << 8);
      at += 2;
      SyncEvent event;
      ASSERT_TRUE(decodeRecord(std::span(bytes).subspan(at, length), event));
      at += length;
      TintaBody body;
      body.kind = EventKind::Star;
      body.course.fill(7);
      body.uid = 1;
      body.enabled = eventIndex == 0;
      std::array<uint8_t, MAX_TINTA_BODY_SIZE> encoded{};
      const auto bodyLength = encodeTintaBody(body, encoded);
      ASSERT_EQ(journal.append(event, std::span(encoded).first(bodyLength)), TintaJournalResult::Ok);
    }
  }
  HalJournalCausalAuditSession audit;
  Digest frontier{};
  ASSERT_TRUE(audit.run(&frontier));
  EXPECT_TRUE(std::equal(frontier.begin(), frontier.end(), bytes.end() - 32));
  class Catalog final : public TintaSubjectCatalog {
   public:
    TintaSubjectMembership result = TintaSubjectMembership::Present;
    unsigned calls = 0;
    TintaSubjectMembership contains(EventKind kind, uint32_t uid) override {
      ++calls;
      EXPECT_EQ(kind, EventKind::Star);
      EXPECT_EQ(uid, 1u);
      return result;
    }
  } catalog;
  Identity course{};
  course.fill(7);
  ASSERT_TRUE(audit.run(&frontier, &course, &catalog));
  EXPECT_EQ(catalog.calls, 2u);
  EXPECT_TRUE(std::equal(frontier.begin(), frontier.end(), bytes.end() - 32));
  const auto valid = frontier;
  catalog.result = TintaSubjectMembership::Missing;
  EXPECT_FALSE(audit.run(&frontier, &course, &catalog));
  EXPECT_EQ(frontier, valid);
  catalog.result = TintaSubjectMembership::IoError;
  EXPECT_FALSE(audit.run(&frontier, &course, &catalog));
  EXPECT_EQ(frontier, valid);
  course.fill(8);
  ASSERT_TRUE(audit.run(&frontier, &course, &catalog));
  EXPECT_EQ(frontier, valid);
  const auto beforeInvalidArguments = state.files;
  EXPECT_FALSE(audit.run(&frontier, &course));
  EXPECT_FALSE(audit.run(&frontier, nullptr, &catalog));
  course.fill(0);
  EXPECT_FALSE(audit.run(&frontier, &course, &catalog));
  EXPECT_EQ(state.files, beforeInvalidArguments);
  EXPECT_EQ(frontier, valid);
  const auto preserved = frontier;
  state.failSync = true;
  EXPECT_FALSE(audit.run(&frontier));
  EXPECT_EQ(frontier, preserved);
}

TEST(HalTintaJournalStorageTest, ExtendedBookmarkJournalUsesHalAndPassesStartupAudit) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  state.directories[TINTA_JOURNAL_DIRECTORY] = {};
  HalTintaJournalStorage storage;
  std::array<uint8_t, TintaJournal::EXTENDED_RECORD_SIZE> scratch{};
  TintaJournal journal(storage, scratch);
  ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
  std::array<uint8_t, MAX_BOOKMARK_BODY_SIZE> body{};
  body[0] = 1;
  body[1] = 2;
  body[2] = 7;
  body[24] = 128;
  std::fill(body.begin() + 26, body.begin() + 154, 'a');
  body[155] = 2;
  std::fill(body.begin() + 156, body.end(), 'b');
  SyncEvent event;
  event.identity.origin.fill(1);
  event.identity.epoch = event.identity.sequence = 1;
  event.storageGeneration.fill(2);
  event.resource.fill(3);
  event.kind = EventKind::BookmarkPut;
  ASSERT_TRUE(storage.digest(body, event.bodyHash));
  ASSERT_EQ(journal.append(event, body), TintaJournalResult::Ok);
  EXPECT_EQ(state.files[TINTA_JOURNAL_EVENTS].size(), 1024u);
  HalCompanionFileLookup lookup;
  EXPECT_TRUE(auditExistingCompanionJournal(lookup));
}

TEST(HalTintaJournalStorageTest, MigrationCandidateCopiesThroughIndependentHalFiles) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  state.directories[TINTA_JOURNAL_DIRECTORY] = {};
  state.directories[MIGRATION_TINTA_JOURNAL_PATHS.directory] = {};
  HalTintaJournalStorage active;
  std::array<uint8_t, 512> oldScratch{};
  TintaJournal source(active, oldScratch);
  ASSERT_EQ(source.open(), TintaJournalResult::Ok);
  TintaBody body;
  body.kind = EventKind::Star;
  body.course.fill(7);
  body.uid = 1;
  body.enabled = true;
  std::array<uint8_t, MAX_TINTA_BODY_SIZE> encoded{};
  const auto length = encodeTintaBody(body, encoded);
  SyncEvent event;
  event.identity.origin.fill(1);
  event.identity.epoch = event.identity.sequence = 1;
  event.storageGeneration.fill(2);
  event.resource.fill(3);
  event.kind = EventKind::Star;
  ASSERT_TRUE(active.digest(std::span(encoded).first(length), event.bodyHash));
  ASSERT_EQ(source.append(event, std::span(encoded).first(length)), TintaJournalResult::Ok);
  const auto original = state.files;
  class Index final : public JournalIdentityIndex {
   public:
    EventIdentity expected{};
    JournalIdentityLookup find(const EventIdentity& identity, uint32_t& record) override {
      if (identity != expected) return JournalIdentityLookup::Missing;
      record = 0;
      return JournalIdentityLookup::Found;
    }
  } index;
  index.expected = event.identity;
  HalTintaJournalStorage candidate(TintaJournalLocation::MigrationCandidate);
  std::array<uint8_t, 1024> scratch{};
  TintaJournal destination(candidate, scratch);
  ASSERT_EQ(destination.open(), TintaJournalResult::Ok);
  JournalMigration migration;
  ASSERT_EQ(migration.copy(source, destination, index), TintaJournalResult::Ok);
  EXPECT_EQ(state.files[MIGRATION_TINTA_JOURNAL_PATHS.events].size(), 1024u);
  EXPECT_EQ(state.files[MIGRATION_TINTA_JOURNAL_PATHS.headerA][3], 3);
  for (const auto* path : {TINTA_JOURNAL_EVENTS, TINTA_JOURNAL_HEADER_A, TINTA_JOURNAL_HEADER_B}) {
    if (original.contains(path))
      EXPECT_EQ(state.files[path], original.at(path));
    else
      EXPECT_FALSE(state.files.contains(path));
  }
  ASSERT_EQ(destination.open(), TintaJournalResult::Ok);
  EXPECT_EQ(migration.copy(source, destination, index), TintaJournalResult::Ok);
  HalJournalCausalAuditSession candidateAudit(TintaJournalLocation::MigrationCandidate);
  JournalMigrationIntent authorization;
  ASSERT_TRUE(candidateAudit.run(&authorization.frontier));
  authorization.count = 1;
  HalJournalMigrationPublicationStorage publication;
  EXPECT_TRUE(publication.verify(JournalMigrationDirectory::Active, authorization, 512));
  EXPECT_TRUE(publication.verify(JournalMigrationDirectory::Candidate, authorization, 1024));
  EXPECT_FALSE(publication.verify(JournalMigrationDirectory::Candidate, authorization, 512));
  ++authorization.count;
  EXPECT_FALSE(publication.verify(JournalMigrationDirectory::Candidate, authorization, 1024));
  state.directories[BACKUP_TINTA_JOURNAL_PATHS.directory] = {};
  EXPECT_FALSE(publication.verify(JournalMigrationDirectory::Backup, authorization, 512));
  EXPECT_FALSE(state.files.contains(BACKUP_TINTA_JOURNAL_PATHS.events));

  const auto beforeInvalid = state.files;
  HalTintaJournalStorage invalid(static_cast<TintaJournalLocation>(99));
  uint32_t size = 123;
  EXPECT_FALSE(invalid.size(size));
  EXPECT_EQ(size, 123u);
  EXPECT_EQ(state.files, beforeInvalid);
}

TEST(HalTintaJournalStorageTest, MigrationIntentStagesReadbackAndRejectsConflictingOwnership) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  HalJournalMigrationIntentStore store;
  JournalMigrationIntent intent;
  intent.count = 2;
  intent.frontier.fill(7);
  JournalMigrationIntent loaded;
  loaded.count = 123;
  ASSERT_EQ(store.load(loaded), JournalMigrationPresence::Missing);
  EXPECT_EQ(loaded.count, 123u);
  state.failSync = true;
  EXPECT_FALSE(store.persist(intent));
  EXPECT_FALSE(state.files.contains(HalJournalMigrationIntentStore::PATH));
  state.failSync = false;
  state.failRenameAfter = state.renames + 1;
  EXPECT_FALSE(store.persist(intent));
  EXPECT_TRUE(state.files.contains(HalJournalMigrationIntentStore::PATH));
  state.failRenameAfter = 0;
  ASSERT_TRUE(store.persist(intent));
  ASSERT_EQ(store.load(loaded), JournalMigrationPresence::Present);
  EXPECT_EQ(loaded.count, intent.count);
  EXPECT_EQ(loaded.frontier, intent.frontier);
  const auto original = state.files;
  auto conflict = intent;
  ++conflict.count;
  EXPECT_FALSE(store.persist(conflict));
  EXPECT_FALSE(store.clear(conflict));
  EXPECT_EQ(state.files, original);
  ASSERT_TRUE(store.clear(intent));
  EXPECT_TRUE(store.clear(intent));
  state.files[HalJournalMigrationIntentStore::PATH] = {1, 2, 3};
  EXPECT_EQ(store.load(loaded), JournalMigrationPresence::IoError);
  EXPECT_FALSE(store.persist(intent));
  EXPECT_EQ(state.files[HalJournalMigrationIntentStore::PATH], std::vector<uint8_t>({1, 2, 3}));
}

TEST(HalTintaJournalStorageTest, DirectoryPublicationRecoversEveryRenameFailureWithLegacyBackup) {
  auto& state = inventory_hal_test::state;
  for (unsigned failedStep = 1; failedStep <= 2; ++failedStep) {
    for (bool after : {false, true}) {
      state = {};
      state.enumerateFileMap = true;
      state.directories[TINTA_JOURNAL_DIRECTORY] = {};
      state.directories[MIGRATION_TINTA_JOURNAL_PATHS.directory] = {};
      {
        HalTintaJournalStorage active;
        std::array<uint8_t, 512> scratch{};
        TintaJournal journal(active, scratch);
        ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
      }
      const auto legacyHeader = state.files.at(TINTA_JOURNAL_HEADER_B);
      {
        HalTintaJournalStorage candidate(TintaJournalLocation::MigrationCandidate);
        std::array<uint8_t, 1024> scratch{};
        TintaJournal journal(candidate, scratch);
        ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
      }
      JournalMigrationIntent intent;
      {
        HalJournalCausalAuditSession audit;
        ASSERT_TRUE(audit.run(&intent.frontier));
      }
      HalJournalMigrationIntentStore intents;
      ASSERT_TRUE(intents.persist(intent));
      if (after)
        state.failDirectoryRenameAfter = failedStep;
      else
        state.failDirectoryRenameBefore = failedStep;
      HalJournalMigrationPublicationStorage publication;
      EXPECT_EQ(recoverJournalMigration(publication), JournalMigrationPublicationResult::IoError);
      EXPECT_TRUE(state.files.contains(HalJournalMigrationIntentStore::PATH));
      state.failDirectoryRenameBefore = state.failDirectoryRenameAfter = 0;
      HalCompanionFileLookup startupLookup;
      ASSERT_TRUE(recoverExistingJournalMigration(startupLookup));
      EXPECT_EQ(state.files.at(BACKUP_TINTA_JOURNAL_PATHS.headerB), legacyHeader);
      EXPECT_EQ(state.files.at(TINTA_JOURNAL_HEADER_B)[3], 3);
      EXPECT_FALSE(state.directories.contains(MIGRATION_TINTA_JOURNAL_PATHS.directory));
      EXPECT_FALSE(state.files.contains(HalJournalMigrationIntentStore::PATH));
      EXPECT_EQ(recoverJournalMigration(publication), JournalMigrationPublicationResult::NoPending);
      EXPECT_EQ(state.directoryRenames, after ? 2u : 3u);
    }
  }
}

TEST(HalTintaJournalStorageTest, StartupMigrationRejectsMalformedIntentAndSkipsAbsentOne) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  HalCompanionFileLookup lookup;
  ASSERT_TRUE(recoverExistingJournalMigration(lookup));
  EXPECT_TRUE(state.files.empty());
  state.files[HalJournalMigrationIntentStore::PATH] = {1, 2, 3};
  const auto before = state.files;
  EXPECT_FALSE(recoverExistingJournalMigration(lookup));
  EXPECT_EQ(state.files, before);
  EXPECT_EQ(state.directoryRenames, 0u);
}

TEST(HalTintaJournalStorageTest, ExplicitMigrationOrchestratesCopyAuthorizationAndPublication) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  state.directories[TINTA_JOURNAL_DIRECTORY] = {};
  state.directories[MIGRATION_TINTA_JOURNAL_PATHS.directory] = {};
  {
    HalTintaJournalStorage storage;
    std::array<uint8_t, 512> scratch{};
    TintaJournal journal(storage, scratch);
    ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
    TintaBody body;
    body.kind = EventKind::Star;
    body.course.fill(7);
    body.uid = 1;
    std::array<uint8_t, MAX_TINTA_BODY_SIZE> encoded{};
    const auto length = encodeTintaBody(body, encoded);
    SyncEvent event;
    event.identity.origin.fill(1);
    event.identity.epoch = event.identity.sequence = 1;
    event.storageGeneration.fill(2);
    event.resource.fill(3);
    event.kind = EventKind::Star;
    ASSERT_TRUE(storage.digest(std::span(encoded).first(length), event.bodyHash));
    ASSERT_EQ(journal.append(event, std::span(encoded).first(length)), TintaJournalResult::Ok);
  }
  const auto legacy = state.files.at(TINTA_JOURNAL_EVENTS);
  HalJournalMigrationSession session;
  state.failDirectoryRenameAfter = 1;
  EXPECT_FALSE(session.run());
  EXPECT_TRUE(state.files.contains(HalJournalMigrationIntentStore::PATH));
  state.failDirectoryRenameAfter = 0;
  ASSERT_TRUE(session.run());
  EXPECT_EQ(state.files.at(BACKUP_TINTA_JOURNAL_PATHS.events), legacy);
  EXPECT_EQ(state.files.at(TINTA_JOURNAL_EVENTS).size(), 1024u);
  EXPECT_FALSE(state.files.contains(HalJournalMigrationIntentStore::PATH));
  ASSERT_TRUE(session.run());
  EXPECT_EQ(state.directoryRenames, 2u);
  HalJournalFormatInventory formats;
  uint8_t versions = 99;
  const auto persisted = state.files;
  ASSERT_TRUE(formats.inspect(versions));
  EXPECT_EQ(versions, 6);
  EXPECT_EQ(state.files, persisted);
  state.files[BACKUP_TINTA_JOURNAL_PATHS.headerB][0] ^= 1;
  const auto corrupted = state.files;
  EXPECT_FALSE(formats.inspect(versions));
  EXPECT_EQ(versions, 6);
  EXPECT_EQ(state.files, corrupted);
}

TEST(HalTintaJournalStorageTest, ReplayVisitsUseBitsAndResetStaleWorkingState) {
  auto& state = inventory_hal_test::state;
  state = {};
  HalJournalReplayVisits visits;
  ASSERT_TRUE(visits.reset(513));
  EXPECT_EQ(state.files[HalJournalReplayVisits::PATH].size(), 65u);
  for (const uint32_t record : {0u, 7u, 8u, 511u, 512u}) ASSERT_TRUE(visits.mark(record));
  for (uint32_t record = 0; record < 513; ++record) {
    bool done = false;
    ASSERT_TRUE(visits.visited(record, done));
    EXPECT_EQ(done, record == 0 || record == 7 || record == 8 || record == 511 || record == 512);
  }
  EXPECT_GT(state.yields, 0u);
  ASSERT_TRUE(visits.reset(9));
  EXPECT_EQ(state.files[HalJournalReplayVisits::PATH].size(), 2u);
  for (uint32_t record = 0; record < 9; ++record) {
    bool done = true;
    ASSERT_TRUE(visits.visited(record, done));
    EXPECT_FALSE(done);
  }
  bool unchanged = true;
  EXPECT_FALSE(visits.visited(9, unchanged));
  EXPECT_TRUE(unchanged);
  EXPECT_FALSE(visits.mark(0));
  ASSERT_TRUE(visits.reset(0));
  EXPECT_TRUE(state.files[HalJournalReplayVisits::PATH].empty());
  EXPECT_FALSE(visits.visited(0, unchanged));
}

TEST(HalTintaJournalStorageTest, ReplayVisitsFailClosedOnReadWriteAndSyncErrors) {
  auto& state = inventory_hal_test::state;
  for (unsigned fault = 0; fault < 3; ++fault) {
    SCOPED_TRACE(fault);
    state = {};
    HalJournalReplayVisits visits;
    ASSERT_TRUE(visits.reset(8));
    if (fault == 0) state.readErrorPath = HalJournalReplayVisits::PATH;
    if (fault == 1) state.failWrite = true;
    if (fault == 2) state.failSync = true;
    EXPECT_FALSE(visits.mark(3));
    state.readErrorPath.clear();
    state.failWrite = state.failSync = false;
    bool unchanged = true;
    EXPECT_FALSE(visits.visited(3, unchanged));
    EXPECT_TRUE(unchanged);
    ASSERT_TRUE(visits.reset(8));
    ASSERT_TRUE(visits.visited(3, unchanged));
    EXPECT_FALSE(unchanged);
  }
}

TEST(HalTintaJournalStorageTest, RebuildReplayExcludesExactUndoneReviewAndKeepsConcurrentReview) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  state.directories[TINTA_JOURNAL_DIRECTORY] = {};
  {
    HalTintaJournalStorage storage;
    std::array<uint8_t, 512> scratch{};
    TintaJournal journal(storage, scratch);
    ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
    TintaBody body;
    body.kind = EventKind::Review;
    body.course.fill(7);
    body.uid = 1;
    body.grade = 3;
    SyncEvent event;
    event.identity.origin.fill(1);
    event.identity.epoch = event.identity.sequence = 1;
    event.storageGeneration.fill(2);
    event.resource.fill(3);
    event.studyDay = 5;
    const auto target = event.identity;
    std::array<uint8_t, MAX_TINTA_BODY_SIZE> bytes{};
    for (unsigned at = 0; at < 5; ++at) {
      if (at == 1) event.identity.origin.fill(4);
      if (at == 2) {
        event.identity = target;
        event.identity.sequence = 2;
        event.ancestorCount = 1;
        event.ancestors[0] = target;
        body.kind = EventKind::UndoReview;
        body.undoTarget = target;
      }
      if (at == 3) {
        event.ancestors[0] = event.identity;
        event.identity.sequence = 3;
        body.kind = EventKind::Star;
        body.enabled = true;
      }
      if (at == 4) {
        event.ancestors[0] = event.identity;
        event.ancestorCount = 2;
        event.ancestors[1] = target;
        event.identity.sequence = 4;
        body.kind = EventKind::UndoReview;
        body.undoTarget = target;
      }
      event.kind = body.kind;
      const auto length = encodeTintaBody(body, bytes);
      ASSERT_NE(length, 0u);
      ASSERT_TRUE(storage.digest(std::span(bytes).first(length), event.bodyHash));
      if (body.kind == EventKind::Review) {
        std::array<uint8_t, 6> configuration{};
        ASSERT_EQ(encodeTintaConfiguration(body.configuration, configuration), configuration.size());
        event.schedulerVersion = 1;
        ASSERT_TRUE(storage.digest(configuration, event.schedulerConfiguration));
      } else {
        event.schedulerVersion = 0;
        event.schedulerConfiguration.fill(0);
      }
      ASSERT_EQ(journal.append(event, std::span(bytes).first(length)), TintaJournalResult::Ok);
    }
  }
  HalJournalCausalAuditSession session;
  ASSERT_TRUE(session.run());
  std::vector<uint32_t> records;
  ASSERT_TRUE(session.replay(
      &records,
      [](void* context, uint32_t record, const SyncEvent&, std::span<const uint8_t>, bool undone) {
        if (!undone) static_cast<std::vector<uint32_t>*>(context)->push_back(record);
        return true;
      },
      true));
  EXPECT_EQ(records, (std::vector<uint32_t>{3, 1}));
  class Catalog final : public TintaSubjectCatalog {
   public:
    TintaSubjectMembership result = TintaSubjectMembership::Present;
    TintaSubjectMembership contains(EventKind, uint32_t uid) override {
      return uid == 1 ? result : TintaSubjectMembership::Missing;
    }
  } catalog;
  Identity course{};
  course.fill(7);
  HalTintaReplaySession rebuild;
  EXPECT_EQ(rebuild.workingStore(), nullptr);
  ASSERT_TRUE(rebuild.run(course, catalog));
  ASSERT_NE(rebuild.workingStore(), nullptr);
  ASSERT_NE(rebuild.journalFrontier(), nullptr);
  tinta::core::ItemState item;
  ASSERT_TRUE(rebuild.workingStore()->item(1, item));
  EXPECT_FALSE(item.isNew());
  EXPECT_NE(item.flags & tinta::core::item_flag::kStarred, 0u);
  TintaReplayDay totals;
  ASSERT_TRUE(rebuild.workingStore()->day(5, totals));
  EXPECT_EQ(totals.gradedReviews, 1u);
  EXPECT_EQ(totals.newItems, 1u);
  EXPECT_EQ(totals.correctReviews, 1u);
  const auto frontier = *rebuild.journalFrontier();
  std::array<uint8_t, 80> exportScratch{};
  HalTintaReplayExport exported;
  ASSERT_TRUE(exported.run(*rebuild.workingStore(), course, 5, exportScratch));
  Identity generation{}, snapshot{};
  Digest pack{};
  generation.fill(2);
  snapshot.fill(3);
  pack.fill(4);
  std::array<uint8_t, TINTA_DERIVED_MANIFEST_SIZE> manifestBytes{};
  ASSERT_TRUE(exported.manifest(generation, pack, frontier, snapshot, 1, manifestBytes));
  TintaDerivedManifestView manifest;
  ASSERT_TRUE(manifest.decode(manifestBytes));
  const auto candidates = state.files;
  HalTintaDerivedJournalProof proof(course, generation, pack, catalog, exportScratch);
  ASSERT_TRUE(HalTintaDerivedJournalProof::callback(&proof, manifest));
  for (unsigned at = 0; at < 5; ++at) {
    std::array<char, COURSE_STATE_PATH_SIZE> path{};
    ASSERT_TRUE(tintaDerivedFilePath(course, static_cast<TintaDerivedFile>(at), TintaDerivedRole::Candidate, path));
    EXPECT_EQ(state.files.at(path.data()), candidates.at(path.data()));
  }
  auto corrupt = manifestBytes;
  corrupt[136] ^= 1;
  tinta_body_detail::write(corrupt, 328, tinta::core::crc32(corrupt.data(), 328), 4);
  TintaDerivedManifestView different;
  ASSERT_TRUE(different.decode(corrupt));
  EXPECT_FALSE(proof.prove(different));
  corrupt = manifestBytes;
  corrupt[52] ^= 1;
  tinta_body_detail::write(corrupt, 328, tinta::core::crc32(corrupt.data(), 328), 4);
  ASSERT_TRUE(different.decode(corrupt));
  EXPECT_FALSE(proof.prove(different));
  std::array<uint8_t, TINTA_DERIVED_MANIFEST_SIZE> publicationScratch{};
  std::array<char, COURSE_STATE_PATH_SIZE> interruptedPath{};
  ASSERT_TRUE(tintaDerivedFilePath(course, TintaDerivedFile::Items, TintaDerivedRole::Candidate, interruptedPath));
  state.failRenameAfterSource = interruptedPath.data();
  EXPECT_EQ(publishProvenTintaDerived(course, generation, pack, catalog, manifestBytes, publicationScratch),
            TintaPublicationResult::IoError);
  state.failRenameAfterSource.clear();
  ASSERT_EQ(publishProvenTintaDerived(course, generation, pack, catalog, manifestBytes, publicationScratch),
            TintaPublicationResult::Ok);
  for (unsigned at = 0; at < 5; ++at) {
    std::array<char, COURSE_STATE_PATH_SIZE> path{};
    ASSERT_TRUE(tintaDerivedFilePath(course, static_cast<TintaDerivedFile>(at), TintaDerivedRole::Active, path));
    ASSERT_TRUE(state.files.contains(path.data()));
    EXPECT_EQ(state.files.at(path.data()).size(), manifest.length(static_cast<TintaDerivedFile>(at)));
    ASSERT_TRUE(tintaDerivedFilePath(course, static_cast<TintaDerivedFile>(at), TintaDerivedRole::Candidate, path));
    EXPECT_FALSE(state.files.contains(path.data()));
  }
  EXPECT_EQ(publishProvenTintaDerived(course, generation, pack, catalog, manifestBytes, publicationScratch),
            TintaPublicationResult::Ok);
  ASSERT_TRUE(rebuild.run(course, catalog));
  EXPECT_EQ(*rebuild.journalFrontier(), frontier);
  catalog.result = TintaSubjectMembership::Missing;
  EXPECT_FALSE(rebuild.run(course, catalog));
  EXPECT_EQ(rebuild.workingStore(), nullptr);
  EXPECT_EQ(rebuild.journalFrontier(), nullptr);
}

TEST(HalTintaJournalStorageTest, ReplayStoreUpdatesRecordsAndKeepsNamespacesSeparate) {
  auto& state = inventory_hal_test::state;
  state = {};
  Identity course{};
  course.fill(7);
  HalTintaReplayStore store;
  ASSERT_TRUE(store.begin(course));
  tinta::core::ItemState item;
  ASSERT_TRUE(store.item(1, item));
  EXPECT_TRUE(item.isNew());
  item.flags = tinta::core::item_flag::kStarred;
  ASSERT_TRUE(store.putItem(item));
  TintaReplayDay totals;
  totals.gradedReviews = totals.newItems = totals.correctReviews = 1;
  totals.responseMilliseconds = 1234;
  ASSERT_TRUE(store.putDay(1, totals));
  ASSERT_TRUE(store.completion(EventKind::LessonComplete, 1, true));
  ASSERT_TRUE(store.completion(EventKind::ReadingComplete, 1, false));
  ASSERT_TRUE(store.item(1, item));
  EXPECT_EQ(item.flags, tinta::core::item_flag::kStarred);
  TintaReplayDay loaded;
  ASSERT_TRUE(store.day(1, loaded));
  EXPECT_EQ(loaded.responseMilliseconds, 1234u);
  bool completed = false;
  ASSERT_TRUE(store.completed(EventKind::LessonComplete, 1, completed));
  EXPECT_TRUE(completed);
  ASSERT_TRUE(store.completed(EventKind::ReadingComplete, 1, completed));
  EXPECT_FALSE(completed);
  const auto extent = state.files.begin()->second.size();
  ASSERT_TRUE(store.completion(EventKind::LessonComplete, 1, false));
  EXPECT_EQ(state.files.begin()->second.size(), extent);
  state.failSync = true;
  EXPECT_FALSE(store.putItem(item));
  state.failSync = false;
  EXPECT_FALSE(store.item(1, item));
  ASSERT_TRUE(store.begin(course));
  ASSERT_TRUE(store.item(1, item));
  EXPECT_TRUE(item.isNew());
  EXPECT_EQ(item.flags, 0u);
}

TEST(HalTintaJournalStorageTest, ReplayStoreRejectsCorruptedCourseHeaderAndRecord) {
  auto& state = inventory_hal_test::state;
  for (const bool header : {false, true}) {
    state = {};
    Identity course{};
    course.fill(7);
    HalTintaReplayStore store;
    ASSERT_TRUE(store.begin(course));
    ASSERT_TRUE(store.putItem(tinta::core::ItemState::fresh(1)));
    auto& bytes = state.files.begin()->second;
    bytes[header ? 4 : 32] ^= 1;
    auto item = tinta::core::ItemState::fresh(9);
    EXPECT_FALSE(store.item(1, item));
    EXPECT_EQ(item.uid, 9u);
    EXPECT_FALSE(store.putItem(item));
  }
}

TEST(HalTintaJournalStorageTest, ReplayStoreEnumeratesCanonicalKeysIncludingStudyDayZero) {
  auto& state = inventory_hal_test::state;
  state = {};
  Identity course{};
  course.fill(7);
  HalTintaReplayStore store;
  ASSERT_TRUE(store.begin(course));
  for (uint32_t uid : {9u, 1u, 4u}) ASSERT_TRUE(store.putItem(tinta::core::ItemState::fresh(uid)));
  ASSERT_TRUE(store.putDay(5, {}));
  ASSERT_TRUE(store.putDay(0, {}));
  ASSERT_TRUE(store.completion(EventKind::LessonComplete, 2, false));
  bool found = false;
  uint32_t key = 99;
  bool previous = false;
  for (uint32_t expected : {1u, 4u, 9u}) {
    ASSERT_TRUE(store.nextKey(HalTintaReplayStore::Kind::Item, previous, key, key, found));
    ASSERT_TRUE(found);
    EXPECT_EQ(key, expected);
    previous = true;
  }
  ASSERT_TRUE(store.nextKey(HalTintaReplayStore::Kind::Item, true, key, key, found));
  EXPECT_FALSE(found);
  EXPECT_EQ(key, 9u);
  ASSERT_TRUE(store.nextKey(HalTintaReplayStore::Kind::Day, false, 0, key, found));
  EXPECT_TRUE(found);
  EXPECT_EQ(key, 0u);
  ASSERT_TRUE(store.nextKey(HalTintaReplayStore::Kind::Day, true, key, key, found));
  EXPECT_TRUE(found);
  EXPECT_EQ(key, 5u);
  ASSERT_TRUE(store.nextKey(HalTintaReplayStore::Kind::Reading, false, 0, key, found));
  EXPECT_FALSE(found);
  EXPECT_EQ(key, 5u);
  state.readErrorPath = state.files.begin()->first;
  found = true;
  EXPECT_FALSE(store.nextKey(HalTintaReplayStore::Kind::Item, false, 0, key, found));
  EXPECT_TRUE(found);
  EXPECT_EQ(key, 5u);
}

TEST(HalTintaJournalStorageTest, ReplayItemExportWritesCanonicalSortedSnapshotAndWithholdsFailedOutput) {
  auto& state = inventory_hal_test::state;
  state = {};
  Identity course{};
  course.fill(7);
  HalTintaReplayStore store;
  ASSERT_TRUE(store.begin(course));
  for (uint32_t uid : {9u, 1u, 4u}) ASSERT_TRUE(store.putItem(tinta::core::ItemState::fresh(uid)));
  TintaReplayDay totals;
  totals.newItems = totals.reviews = totals.gradedReviews = 70000;
  ASSERT_TRUE(store.putDay(5, totals));
  HalTintaReplayItemExport output;
  ASSERT_TRUE(output.run(store, course, 5));
  ASSERT_NE(output.verifiedPath(), nullptr);
  const auto original = state.files.at(output.verifiedPath());
  ASSERT_EQ(original.size(), 1072u);
  EXPECT_EQ(tinta::core::getU32(original.data() + 12), 3u);
  EXPECT_EQ(tinta::core::getU16(original.data() + 22), UINT16_MAX);
  EXPECT_EQ(tinta::core::getU16(original.data() + 24), UINT16_MAX);
  EXPECT_EQ(tinta::core::getU32(original.data() + 1024), 1u);
  EXPECT_EQ(tinta::core::getU32(original.data() + 1040), 4u);
  EXPECT_EQ(tinta::core::getU32(original.data() + 1056), 9u);
  ASSERT_TRUE(output.run(store, course, 5));
  EXPECT_EQ(state.files.at(output.verifiedPath()), original);
  state.failSync = true;
  EXPECT_FALSE(output.run(store, course, 5));
  EXPECT_EQ(output.verifiedPath(), nullptr);
  state.failSync = false;
  auto different = course;
  different.fill(8);
  EXPECT_FALSE(output.run(store, different, 5));
  EXPECT_EQ(output.verifiedPath(), nullptr);
}

TEST(HalTintaJournalStorageTest, ReplayCompletionExportMatchesGoldenBytesAndSeparatesNamespaces) {
  auto& state = inventory_hal_test::state;
  state = {};
  Identity course{};
  course.fill(7);
  HalTintaReplayStore store;
  ASSERT_TRUE(store.begin(course));
  ASSERT_TRUE(store.completion(EventKind::LessonComplete, 9, true));
  ASSERT_TRUE(store.completion(EventKind::LessonComplete, 4, false));
  ASSERT_TRUE(store.completion(EventKind::LessonComplete, 1, true));
  ASSERT_TRUE(store.completion(EventKind::ReadingComplete, 4, true));
  HalTintaReplayCompletionExport output;
  ASSERT_TRUE(output.run(store, course, TintaCompletionKind::Lessons));
  ASSERT_NE(output.verifiedPath(), nullptr);
  const std::vector<uint8_t> golden = {84, 67, 83, 49, 1, 0, 0, 0, 2,   0,   0,  0,
                                       1,  0,  0,  0,  9, 0, 0, 0, 162, 192, 85, 73};
  EXPECT_EQ(state.files.at(output.verifiedPath()), golden);
  ASSERT_TRUE(output.run(store, course, TintaCompletionKind::Readings));
  const auto& reading = state.files.at(output.verifiedPath());
  ASSERT_EQ(reading.size(), 20u);
  EXPECT_EQ(reading[4], 2u);
  EXPECT_EQ(tinta::core::getU32(reading.data() + 12), 4u);
  ASSERT_TRUE(store.completion(EventKind::ReadingComplete, 4, false));
  ASSERT_TRUE(output.run(store, course, TintaCompletionKind::Readings));
  EXPECT_EQ(state.files.at(output.verifiedPath()).size(), 16u);
  state.failWrite = true;
  EXPECT_FALSE(output.run(store, course, TintaCompletionKind::Lessons));
  EXPECT_EQ(output.verifiedPath(), nullptr);
}

TEST(HalTintaJournalStorageTest, ReplayDayExportMatchesGoldenSplitTotalsAndRoundsOncePerDay) {
  auto& state = inventory_hal_test::state;
  state = {};
  Identity course{};
  course.fill(7);
  HalTintaReplayStore store;
  ASSERT_TRUE(store.begin(course));
  TintaReplayDay totals;
  totals.gradedReviews = 70000;
  totals.correctReviews = 65000;
  totals.newItems = 65536;
  totals.responseMilliseconds = 1500;
  ASSERT_TRUE(store.putDay(5, totals));
  ASSERT_TRUE(store.putDay(0, {}));
  HalTintaReplayDayExport output;
  ASSERT_TRUE(output.run(store, course));
  const std::vector<uint8_t> golden = {84,  68,  76,  49, 0,   0,   0,   0,   0,   0,   0,  0,  0,   0,
                                       118, 104, 5,   0,  255, 255, 232, 253, 255, 255, 2,  0,  121, 178,
                                       5,   0,   113, 17, 0,   0,   1,   0,   0,   0,   35, 194};
  EXPECT_EQ(state.files.at(output.verifiedPath()), golden);
  totals.responseMilliseconds = (uint64_t{UINT32_MAX} + 1) * 1000;
  ASSERT_TRUE(store.putDay(5, totals));
  EXPECT_FALSE(output.run(store, course));
  EXPECT_EQ(output.verifiedPath(), nullptr);
  ASSERT_TRUE(store.begin(course));
  ASSERT_TRUE(output.run(store, course));
  EXPECT_EQ(state.files.at(output.verifiedPath()), std::vector<uint8_t>({'T', 'D', 'L', '1'}));
}

TEST(HalTintaJournalStorageTest, CompleteReplayExportBindsReceiptsToAllFiveFiles) {
  auto& state = inventory_hal_test::state;
  state = {};
  Identity course{};
  course.fill(7);
  HalTintaReplayStore store;
  ASSERT_TRUE(store.begin(course));
  ASSERT_TRUE(store.putItem(tinta::core::ItemState::fresh(1)));
  ASSERT_TRUE(store.completion(EventKind::LessonComplete, 1, true));
  HalTintaReplayExport output;
  std::array<uint8_t, 80> scratch{};
  ASSERT_TRUE(output.run(store, course, 5, scratch));
  EXPECT_TRUE(output.complete());
  Identity storage{}, snapshot{};
  Digest pack{}, frontier{};
  storage.fill(2);
  snapshot.fill(3);
  pack.fill(4);
  frontier.fill(5);
  std::array<uint8_t, TINTA_DERIVED_MANIFEST_SIZE> manifest{};
  manifest.fill(7);
  const auto unchangedManifest = manifest;
  EXPECT_FALSE(output.manifest(storage, pack, frontier, snapshot, 0, manifest));
  EXPECT_EQ(manifest, unchangedManifest);
  ASSERT_TRUE(output.manifest(storage, pack, frontier, snapshot, 1, manifest));
  TintaDerivedManifestView view;
  ASSERT_TRUE(view.decode(manifest));
  EXPECT_TRUE(view.matches(course, storage, pack, frontier));
  EXPECT_EQ(view.studyDay(), 5u);
  EXPECT_EQ(view.revision(), 1u);
  EXPECT_TRUE(std::equal(snapshot.begin(), snapshot.end(), view.snapshotIdentity().begin()));
  for (unsigned at = 0; at < 5; ++at) {
    uint64_t length = 99;
    Digest hash{};
    const auto kind = static_cast<TintaDerivedFile>(at);
    ASSERT_TRUE(output.receipt(kind, length, hash));
    std::array<char, COURSE_STATE_PATH_SIZE> path{};
    ASSERT_TRUE(tintaDerivedFilePath(course, kind, TintaDerivedRole::Candidate, path));
    const auto& data = state.files.at(path.data());
    EXPECT_EQ(length, data.size());
    Digest expected{};
    SHA256(data.data(), data.size(), expected.data());
    EXPECT_EQ(hash, expected);
    EXPECT_EQ(view.length(kind), length);
    EXPECT_TRUE(std::equal(hash.begin(), hash.end(), view.hash(kind).begin()));
  }
  state.failSync = true;
  EXPECT_FALSE(output.run(store, course, 5, scratch));
  EXPECT_FALSE(output.complete());
  uint64_t unchanged = 99;
  Digest hash{};
  hash.fill(7);
  EXPECT_FALSE(output.receipt(TintaDerivedFile::Items, unchanged, hash));
  EXPECT_EQ(unchanged, 99u);
  EXPECT_EQ(hash[0], 7u);
}

TEST(HalTintaJournalStorageTest, ReplayProofExportLeavesCandidateAndActiveFilesUnchanged) {
  auto& state = inventory_hal_test::state;
  state = {};
  Identity course{};
  course.fill(7);
  HalTintaReplayStore store;
  ASSERT_TRUE(store.begin(course));
  for (unsigned at = 0; at < 5; ++at) {
    std::array<char, COURSE_STATE_PATH_SIZE> path{};
    ASSERT_TRUE(tintaDerivedFilePath(course, static_cast<TintaDerivedFile>(at), TintaDerivedRole::Candidate, path));
    state.files[path.data()] = {1, 2, 3};
    ASSERT_TRUE(tintaDerivedFilePath(course, static_cast<TintaDerivedFile>(at), TintaDerivedRole::Active, path));
    state.files[path.data()] = {4, 5, 6};
  }
  const auto preserved = state.files;
  HalTintaReplayExport proof(TintaReplayExportTarget::Proof);
  std::array<uint8_t, 80> scratch{};
  ASSERT_TRUE(proof.run(store, course, 5, scratch));
  for (const auto& [path, bytes] : preserved) EXPECT_EQ(state.files.at(path), bytes);
  for (unsigned at = 0; at < 5; ++at) {
    std::array<char, COURSE_STATE_PATH_SIZE> path{};
    ASSERT_TRUE(tintaReplayExportPath(course, static_cast<TintaDerivedFile>(at), TintaReplayExportTarget::Proof, path));
    EXPECT_TRUE(state.files.contains(path.data()));
  }
}

TEST(HalTintaJournalStorageTest, LegacyBackupSessionReconstructsCanonicalTransactionAfterRestart) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  Identity reader{}, generation{}, course{}, transaction{};
  reader[0] = 1;
  generation[0] = 2;
  course[0] = 3;
  transaction[0] = 4;
  std::array<const char*, LEGACY_TINTA_BACKUP_ROLES> sources{};
  sources[0] = "/reviews";
  sources[1] = "/items";
  sources[2] = "/profile";
  for (size_t at = 0; at < 3; ++at) state.files[sources[at]] = std::vector<uint8_t>(80, at + 1);
  {
    auto session = makeUniqueNoThrow<HalLegacyTintaBackupSession>();
    ASSERT_TRUE(session);
    EXPECT_FALSE(session->run());
    ASSERT_TRUE(session->prepare(reader, generation, course, transaction, sources));
    ASSERT_TRUE(session->run());
    ASSERT_TRUE(session->verify());
  }
  const auto saved = state.files;
  auto restarted = makeUniqueNoThrow<HalLegacyTintaBackupSession>();
  ASSERT_TRUE(restarted);
  ASSERT_TRUE(restarted->prepare(reader, generation, course, transaction, sources));
  ASSERT_TRUE(restarted->recover());
  ASSERT_TRUE(restarted->verify());
  EXPECT_EQ(state.files, saved);
  generation[0] ^= 1;
  ASSERT_TRUE(restarted->prepare(reader, generation, course, transaction, sources));
  EXPECT_FALSE(restarted->recover());
  EXPECT_EQ(state.files, saved);
}

TEST(HalTintaJournalStorageTest, LegacyBackupSessionRecoversManifestPublicationWithoutOriginals) {
  auto& state = inventory_hal_test::state;
  for (const bool lostAcknowledgment : {false, true}) {
    state = {};
    state.enumerateFileMap = true;
    Identity reader{}, generation{}, course{}, transaction{};
    reader[0] = 1;
    generation[0] = 2;
    course[0] = 3;
    transaction[0] = 4;
    std::array<const char*, LEGACY_TINTA_BACKUP_ROLES> sources{};
    sources[0] = "/reviews";
    sources[1] = "/items";
    sources[2] = "/profile";
    for (size_t at = 0; at < 3; ++at) state.files[sources[at]] = std::vector<uint8_t>(80, at + 1);
    std::array<char, LEGACY_TINTA_BACKUP_PATH_SIZE> finalPath{}, stagePath{}, intentPath{};
    ASSERT_TRUE(legacyTintaBackupMetadataPath(reader, generation, course, transaction,
                                              LegacyTintaBackupMetadata::Manifest, finalPath));
    ASSERT_TRUE(legacyTintaBackupMetadataPath(reader, generation, course, transaction,
                                              LegacyTintaBackupMetadata::ManifestStage, stagePath));
    ASSERT_TRUE(legacyTintaBackupMetadataPath(reader, generation, course, transaction,
                                              LegacyTintaBackupMetadata::Intent, intentPath));
    if (lostAcknowledgment)
      state.failRenameAfterSource = stagePath.data();
    else
      state.failRename = 5;
    {
      auto session = makeUniqueNoThrow<HalLegacyTintaBackupSession>();
      ASSERT_TRUE(session);
      ASSERT_TRUE(session->prepare(reader, generation, course, transaction, sources));
      EXPECT_FALSE(session->run());
    }
    ASSERT_TRUE(state.files.contains(intentPath.data()));
    EXPECT_EQ(state.files.contains(finalPath.data()), lostAcknowledgment);
    for (size_t at = 0; at < 3; ++at) {
      ASSERT_TRUE(legacyTintaBackupFilePath(reader, generation, course, transaction,
                                            static_cast<LegacyTintaBackupRole>(at), false, stagePath));
      EXPECT_EQ(state.files.at(stagePath.data()), state.files.at(sources[at]));
      state.files.erase(sources[at]);
    }
    state.failRename = 0;
    state.failRenameAfterSource.clear();
    auto restarted = makeUniqueNoThrow<HalLegacyTintaBackupSession>();
    ASSERT_TRUE(restarted);
    ASSERT_TRUE(restarted->prepare(reader, generation, course, transaction, sources));
    if (!lostAcknowledgment) {
      ASSERT_TRUE(legacyTintaBackupMetadataPath(reader, generation, course, transaction,
                                                LegacyTintaBackupMetadata::ManifestStage, stagePath));
      state.files.at(stagePath.data()).resize(100);
      state.files.at(stagePath.data())[0] ^= 1;
      const auto damagedStage = state.files;
      EXPECT_FALSE(restarted->recover());
      EXPECT_EQ(state.files, damagedStage);
      state.files.at(stagePath.data())[0] ^= 1;
    }
    ASSERT_TRUE(restarted->recover());
    ASSERT_TRUE(restarted->verify());
    EXPECT_EQ(state.files.at(finalPath.data()), state.files.at(intentPath.data()));
    const auto recovered = state.files;
    ASSERT_TRUE(restarted->recover());
    EXPECT_EQ(state.files, recovered);
  }
}

TEST(HalTintaJournalStorageTest, CanonicalLegacyBackupRecoveryRebuildsOnlyMatchingPartial) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  Identity reader{}, generation{}, course{}, transaction{};
  reader[0] = 1;
  generation[0] = 2;
  course[0] = 3;
  transaction[0] = 4;
  std::array<const char*, LEGACY_TINTA_BACKUP_ROLES> sources{};
  sources[0] = "/reviews";
  sources[1] = "/items";
  sources[2] = "/profile";
  for (size_t at = 0; at < 3; ++at) state.files[sources[at]] = std::vector<uint8_t>(16384, at + 1);
  state.failRename = 2;
  {
    auto session = makeUniqueNoThrow<HalLegacyTintaBackupSession>();
    ASSERT_TRUE(session);
    ASSERT_TRUE(session->prepare(reader, generation, course, transaction, sources));
    EXPECT_FALSE(session->run());
  }
  state.failRename = 0;
  std::array<char, LEGACY_TINTA_BACKUP_PATH_SIZE> candidate{};
  ASSERT_TRUE(legacyTintaBackupFilePath(reader, generation, course, transaction, LegacyTintaBackupRole::Reviews, true,
                                        candidate));
  state.files.at(candidate.data()).resize(8193);
  state.files.at(candidate.data())[0] ^= 1;
  const auto conflict = state.files;
  auto restarted = makeUniqueNoThrow<HalLegacyTintaBackupSession>();
  ASSERT_TRUE(restarted);
  ASSERT_TRUE(restarted->prepare(reader, generation, course, transaction, sources));
  EXPECT_FALSE(restarted->recover());
  EXPECT_EQ(state.files, conflict);
  state.files.at(candidate.data())[0] ^= 1;
  state.files.at("/reviews").back() ^= 1;
  const auto changedSource = state.files;
  EXPECT_FALSE(restarted->recover());
  EXPECT_EQ(state.files, changedSource);
  state.files.at("/reviews").back() ^= 1;
  ASSERT_TRUE(restarted->recover());
  ASSERT_TRUE(restarted->verify());
  EXPECT_FALSE(state.files.contains(candidate.data()));
  EXPECT_EQ(state.files.at("/reviews"), std::vector<uint8_t>(16384, 1));
}

TEST(HalTintaJournalStorageTest, NativeLegacyBackupSelectionPreservesCourseReadingsDaysAndSession) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  Identity reader{}, generation{}, course{}, transaction{};
  reader[0] = 1;
  generation[0] = 2;
  course[0] = 3;
  transaction[0] = 4;
  std::array<char, COURSE_STATE_DIRECTORY_SIZE> root{};
  ASSERT_TRUE(courseStateDirectory(course, root));
  ASSERT_TRUE(Storage.ensureDirectoryExists(root.data()));
  for (const auto* name :
       {"reviews.log", "items.bin", "profile.bin", "read.bin", "starred.bin", "days.bin", "session.bin"})
    state.files[std::string(root.data()) + "/" + name] = std::vector<uint8_t>(80, 5);
  auto session = makeUniqueNoThrow<HalLegacyTintaBackupSession>();
  ASSERT_TRUE(session);
  ASSERT_TRUE(session->prepareCourse(reader, generation, course, transaction, true));
  ASSERT_TRUE(session->run());
  ASSERT_TRUE(session->verify());
  ASSERT_TRUE(session->matchesCurrentCourse());
  const auto profilePath = std::string(root.data()) + "/profile.bin";
  state.files[profilePath][3] ^= 1;
  const auto changedSource = state.files;
  EXPECT_FALSE(session->matchesCurrentCourse());
  EXPECT_EQ(state.files, changedSource);
  ASSERT_TRUE(session->verify());
  state.files[profilePath][3] ^= 1;
  state.files[profilePath].push_back(1);
  EXPECT_FALSE(session->matchesCurrentCourse());
  state.files[profilePath].pop_back();
  state.readErrorPath = profilePath;
  EXPECT_FALSE(session->matchesCurrentCourse());
  state.readErrorPath.clear();
  state.failClosePath = profilePath;
  EXPECT_FALSE(session->matchesCurrentCourse());
  state.failClosePath.clear();
  ASSERT_TRUE(session->matchesCurrentCourse());
  std::array<char, LEGACY_TINTA_BACKUP_PATH_SIZE> backup{};
  for (const auto role :
       {LegacyTintaBackupRole::Readings, LegacyTintaBackupRole::Days, LegacyTintaBackupRole::Session}) {
    ASSERT_TRUE(legacyTintaBackupFilePath(reader, generation, course, transaction, role, false, backup));
    EXPECT_EQ(state.files.at(backup.data()), std::vector<uint8_t>(80, 5));
  }
  for (const auto* name :
       {"reviews.log", "items.bin", "profile.bin", "read.bin", "starred.bin", "days.bin", "session.bin"})
    state.files.erase(std::string(root.data()) + "/" + name);
  const auto saved = state.files;
  session.reset();
  session = makeUniqueNoThrow<HalLegacyTintaBackupSession>();
  ASSERT_TRUE(session);
  ASSERT_TRUE(session->prepareCourse(reader, generation, course, transaction, true, true));
  ASSERT_TRUE(session->recover());
  ASSERT_TRUE(session->verify());
  EXPECT_EQ(state.files, saved);
  EXPECT_FALSE(session->matchesCurrentCourse());
  EXPECT_EQ(state.files, saved);
  std::array<uint8_t, LEGACY_TINTA_BACKUP_MANIFEST_SIZE> exportedManifest{};
  ASSERT_TRUE(session->readManifest(exportedManifest));
  LegacyTintaBackupManifestView exportedView;
  ASSERT_TRUE(exportedView.decode(exportedManifest));
  EXPECT_TRUE(exportedView.present(LegacyTintaBackupRole::Readings));
  EXPECT_TRUE(exportedView.present(LegacyTintaBackupRole::Days));
  EXPECT_EQ(exportedView.length(LegacyTintaBackupRole::Readings), 80);
  EXPECT_FALSE(session->openExport(LegacyTintaBackupRole::Readings, 81));
  ASSERT_TRUE(session->openExport(LegacyTintaBackupRole::Readings));
  std::array<uint8_t, 31> chunk{};
  size_t written = 99;
  EXPECT_FALSE(session->readExport(1, chunk, written));
  EXPECT_EQ(written, 0);
  EXPECT_FALSE(session->run());
  ASSERT_TRUE(session->readExport(0, chunk, written));
  EXPECT_EQ(written, 31);
  ASSERT_TRUE(session->closeExport());
  ASSERT_TRUE(session->openExport(LegacyTintaBackupRole::Readings, 31));
  ASSERT_TRUE(session->readExport(31, chunk, written));
  EXPECT_EQ(written, 31);
  ASSERT_TRUE(session->readExport(62, chunk, written));
  EXPECT_EQ(written, 18);
  EXPECT_FALSE(session->readExport(80, chunk, written));
  ASSERT_TRUE(session->openExport(LegacyTintaBackupRole::Readings));
  ASSERT_TRUE(legacyTintaBackupFilePath(reader, generation, course, transaction, LegacyTintaBackupRole::Readings, false,
                                        backup));
  state.files.at(backup.data()).back() ^= 1;
  ASSERT_TRUE(session->readExport(0, chunk, written));
  ASSERT_TRUE(session->readExport(31, chunk, written));
  EXPECT_FALSE(session->readExport(62, chunk, written));
  EXPECT_EQ(written, 0);
  state.files.at(backup.data()).back() ^= 1;
  course[0] ^= 1;
  EXPECT_FALSE(session->prepareCourse(reader, generation, course, transaction, true));
  EXPECT_FALSE(session->run());
  EXPECT_EQ(state.files, saved);
}

TEST(HalTintaJournalStorageTest, BackupExchangeCapturesAndExportsWithTransactionBoundReplies) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  Identity reader{};
  reader[0] = 1;
  LegacyBackupRequest request;
  request.course[0] = 3;
  request.generation[0] = 2;
  request.transaction[0] = 4;
  request.bound = true;
  request.operation = LegacyBackupOperation::Capture;
  std::array<char, COURSE_STATE_DIRECTORY_SIZE> root{};
  ASSERT_TRUE(courseStateDirectory(request.course, root));
  ASSERT_TRUE(Storage.ensureDirectoryExists(root.data()));
  for (const auto* name : {"reviews.log", "items.bin", "profile.bin"})
    state.files[std::string(root.data()) + "/" + name] = std::vector<uint8_t>(80, 7);
  auto exchange = makeUniqueNoThrow<HalLegacyBackupExchange>();
  ASSERT_TRUE(exchange);
  std::array<uint8_t, 800> output{};
  LegacyBackupReply reply;
  auto length = exchange->dispatch(request, reader, output);
  ASSERT_TRUE(decodeLegacyBackupReply(std::span<const uint8_t>(output).first(length), reply));
  EXPECT_EQ(reply.result, LegacyBackupResult::Ok);
  state.files[std::string(root.data()) + "/reviews.log"][0] ^= 1;
  exchange.reset();
  exchange = makeUniqueNoThrow<HalLegacyBackupExchange>();
  ASSERT_TRUE(exchange);
  length = exchange->dispatch(request, reader, output);
  ASSERT_TRUE(decodeLegacyBackupReply(std::span<const uint8_t>(output).first(length), reply));
  EXPECT_EQ(reply.result, LegacyBackupResult::Ok);
  request.operation = LegacyBackupOperation::Manifest;
  length = exchange->dispatch(request, reader, output);
  ASSERT_TRUE(decodeLegacyBackupReply(std::span<const uint8_t>(output).first(length), reply));
  EXPECT_EQ(reply.body.size(), LEGACY_TINTA_BACKUP_MANIFEST_SIZE);
  request.operation = LegacyBackupOperation::File;
  request.role = 0;
  request.count = 31;
  for (const uint32_t offset : {0U, 0U, 31U, 62U}) {
    request.offset = offset;
    length = exchange->dispatch(request, reader, output);
    ASSERT_TRUE(decodeLegacyBackupReply(std::span<const uint8_t>(output).first(length), reply));
    EXPECT_EQ(reply.result, LegacyBackupResult::Ok);
    EXPECT_EQ(reply.offset, offset);
    EXPECT_EQ(reply.final, offset == 62);
    EXPECT_EQ(reply.body.size(), offset == 62 ? 18 : 31);
    EXPECT_TRUE(std::all_of(reply.body.begin(), reply.body.end(), [](uint8_t byte) { return byte == 7; }));
  }
  const auto saved = state.files;
  request.transaction[0] ^= 1;
  length = exchange->dispatch(request, reader, output);
  ASSERT_TRUE(decodeLegacyBackupReply(std::span<const uint8_t>(output).first(length), reply));
  EXPECT_EQ(reply.result, LegacyBackupResult::Conflict);
  EXPECT_EQ(state.files, saved);
}

TEST(HalTintaJournalStorageTest, ApplicationReceiptPublicationPreservesConflictsAndRecoversRenameReply) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  constexpr auto path = "/.crosspoint/companion/application-test";
  constexpr auto stage = "/.crosspoint/companion/application-test-next";
  TintaApplicationReceipt value;
  value.event.origin.fill(1);
  value.event.epoch = 2;
  value.event.sequence = 3;
  value.course.fill(4);
  value.generation.fill(5);
  value.resource.fill(6);
  value.before = tinta::core::ItemState::fresh(1);
  value.after = value.before;
  value.after.flags = tinta::core::item_flag::kStarred;
  value.entry = tinta::core::JournalEntry::control(1, tinta::core::JournalEntry::kSetFlags, value.after.flags, 5, 100);
  HalTintaApplicationReceiptStore records(path, stage);
  TintaApplicationReceipt output;
  EXPECT_EQ(records.load(output), TintaApplicationReceiptResult::Missing);
  state.failSync = true;
  EXPECT_EQ(records.persist(value), TintaApplicationReceiptResult::IoError);
  EXPECT_EQ(state.files.count(path), 0U);
  state.failSync = false;
  state.failRenameAfterSource = stage;
  EXPECT_EQ(records.persist(value), TintaApplicationReceiptResult::Ok);
  ASSERT_EQ(records.load(output), TintaApplicationReceiptResult::Ok);
  EXPECT_EQ(output.event.sequence, value.event.sequence);
  const auto renames = state.renames;
  const auto identical = state.files;
  EXPECT_EQ(records.persist(value), TintaApplicationReceiptResult::Ok);
  EXPECT_EQ(state.renames, renames);
  EXPECT_EQ(state.files, identical);
  const auto before = state.files;
  value.generation[0] ^= 1;
  EXPECT_EQ(records.persist(value), TintaApplicationReceiptResult::Conflict);
  EXPECT_EQ(state.files, before);
  state.files[path][10] ^= 1;
  const auto corrupt = state.files;
  EXPECT_EQ(records.persist(value), TintaApplicationReceiptResult::Corrupt);
  EXPECT_EQ(state.files, corrupt);
}

TEST(HalTintaJournalStorageTest, ApplicationReceiptsKeepDistinctEventsAndOwnershipContexts) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  HalTintaJournalStorage storage;
  HalTintaApplicationReceipts records(storage);
  TintaApplicationReceipt value;
  value.event.origin.fill(1);
  value.event.epoch = 2;
  value.event.sequence = 3;
  value.course.fill(4);
  value.generation.fill(5);
  value.resource.fill(6);
  value.before = tinta::core::ItemState::fresh(1);
  value.after = value.before;
  value.after.flags = tinta::core::item_flag::kStarred;
  value.entry = tinta::core::JournalEntry::control(1, tinta::core::JournalEntry::kSetFlags, value.after.flags, 5, 100);
  ASSERT_EQ(records.persist(value), TintaApplicationReceiptResult::Ok);
  EXPECT_TRUE(state.files.contains(
      "/.crosspoint/companion/tinta-applied-fddea07e2fbddfafea95db1ea4660c04e25e5a1d0e155124f908861887f26279"));
  const auto original = value;
  value.event.sequence++;
  ASSERT_EQ(records.persist(value), TintaApplicationReceiptResult::Ok);
  value.course[0] ^= 1;
  ASSERT_EQ(records.persist(value), TintaApplicationReceiptResult::Ok);
  value.generation[0] ^= 1;
  ASSERT_EQ(records.persist(value), TintaApplicationReceiptResult::Ok);
  value.resource[0] ^= 1;
  ASSERT_EQ(records.persist(value), TintaApplicationReceiptResult::Ok);
  EXPECT_EQ(state.files.size(), 5U);
  TintaApplicationReceipt output;
  ASSERT_EQ(records.load(original.event, original.course, original.generation, original.resource, output),
            TintaApplicationReceiptResult::Ok);
  EXPECT_EQ(output.event, original.event);
  EXPECT_EQ(output.resource, original.resource);
  value.entry.time++;
  const auto prior = state.files;
  EXPECT_EQ(records.persist(value), TintaApplicationReceiptResult::Conflict);
  EXPECT_EQ(state.files, prior);
  value.event.sequence++;
  EXPECT_EQ(records.load(value.event, value.course, value.generation, value.resource, output),
            TintaApplicationReceiptResult::Missing);
  EXPECT_EQ(output.event, original.event);
}

TEST(HalTintaJournalStorageTest, ApplicationAcknowledgementRequiresAuthorityAndNativeBytesForReviewUndoAndFlags) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  HalTintaJournalStorage storage;
  std::array<uint8_t, 1024> scratch{};
  TintaJournal journal(storage, scratch);
  ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
  HalTintaApplicationReceipts receipts(storage);
  tinta_test::MemStore local;
  const auto catalog = tinta_test::FakeCatalog::vocab(1, 1);
  tinta::core::Fsrs scheduler;
  tinta::core::ProgressStore progress(local, catalog, scheduler);
  std::array<uint16_t, 2> slots{};
  ASSERT_EQ(progress.open(slots.data(), slots.size(), nullptr, 0), tinta::core::ProgressStore::OpenResult::Created);
  Identity course{}, generation{};
  Digest resource{};
  course.fill(4);
  generation.fill(5);
  resource.fill(6);
  HalTintaApplicationAcknowledge owner(journal, storage, progress, receipts, course, generation, resource);
  TintaBody body;
  body.course = course;
  body.uid = catalog.uidAt(0);
  body.grade = 3;
  body.format = 2;
  body.responseMilliseconds = 1234;
  SyncEvent event;
  event.identity.origin.fill(1);
  event.identity.epoch = 2;
  event.identity.sequence = 1;
  event.storageGeneration = generation;
  event.resource = resource;
  event.studyDay = 5;
  event.kind = EventKind::Review;
  event.schedulerVersion = 1;
  std::array<uint8_t, MAX_TINTA_BODY_SIZE> bytes{};
  std::array<uint8_t, 6> configuration{};
  auto length = encodeTintaBody(body, bytes);
  ASSERT_TRUE(storage.digest(std::span(bytes).first(length), event.bodyHash));
  ASSERT_EQ(encodeTintaConfiguration(body.configuration, configuration), 6U);
  ASSERT_TRUE(storage.digest(configuration, event.schedulerConfiguration));
  ASSERT_EQ(journal.append(event, std::span(bytes).first(length)), TintaJournalResult::Ok);
  const auto result = progress.review(0, tinta::core::Grade::Good, 2, 1234, 5, 100);
  ASSERT_EQ(result.status, tinta::core::ProgressStore::Status::Stored);
  const auto entry = tinta::core::JournalEntry::review(body.uid, tinta::core::Grade::Good, 2, 1234, 5, 100);
  const auto before = state.files;
  EXPECT_FALSE(owner.acknowledge(event.identity, entry, result.before, result.after, 1235));
  EXPECT_EQ(state.files, before);
  local.files["items.bin"][1024 + 8] ^= 1;
  EXPECT_FALSE(owner.acknowledge(event.identity, entry, result.before, result.after, 1234));
  local.files["items.bin"][1024 + 8] ^= 1;
  state.failSync = true;
  EXPECT_FALSE(owner.acknowledge(event.identity, entry, result.before, result.after, 1234));
  state.failSync = false;
  EventIdentity recoveredUndo;
  recoveredUndo.origin.fill(99);
  const auto untouchedUndo = recoveredUndo;
  EXPECT_FALSE(owner.recoverUndo(recoveredUndo));
  EXPECT_EQ(recoveredUndo, untouchedUndo);
  ASSERT_TRUE(owner.acknowledge(event.identity, entry, result.before, result.after, 1234));
  ASSERT_TRUE(owner.recoverUndo(recoveredUndo));
  EXPECT_EQ(recoveredUndo, event.identity);
  local.files["items.bin"][1024 + 8] ^= 1;
  EXPECT_FALSE(owner.recoverUndo(recoveredUndo));
  EXPECT_EQ(recoveredUndo, event.identity);
  local.files["items.bin"][1024 + 8] ^= 1;
  TintaApplicationReceipt loaded;
  ASSERT_EQ(receipts.load(event.identity, course, generation, resource, loaded), TintaApplicationReceiptResult::Ok);
  const auto reviewIdentity = event.identity;
  event.identity.sequence = 2;
  event.ancestorCount = 1;
  event.ancestors[0] = reviewIdentity;
  event.kind = EventKind::UndoReview;
  event.studyDay = 6;
  event.schedulerVersion = 0;
  event.schedulerConfiguration = {};
  body.kind = EventKind::UndoReview;
  body.undoTarget = reviewIdentity;
  length = encodeTintaBody(body, bytes);
  ASSERT_TRUE(storage.digest(std::span(bytes).first(length), event.bodyHash));
  ASSERT_EQ(journal.append(event, std::span(bytes).first(length)), TintaJournalResult::Ok);
  tinta::core::ItemState restored;
  ASSERT_EQ(progress.undo(6, 200, &restored), tinta::core::ProgressStore::Status::Stored);
  const auto undo = tinta::core::JournalEntry::control(body.uid, tinta::core::JournalEntry::kUndo, 0, 6, 200);
  ASSERT_TRUE(owner.acknowledge(event.identity, undo, result.after, restored, 0));
  event.ancestors[0] = event.identity;
  event.identity.sequence = 3;
  event.kind = EventKind::Star;
  body.kind = EventKind::Star;
  body.enabled = true;
  length = encodeTintaBody(body, bytes);
  ASSERT_TRUE(storage.digest(std::span(bytes).first(length), event.bodyHash));
  ASSERT_EQ(journal.append(event, std::span(bytes).first(length)), TintaJournalResult::Ok);
  const auto beforeFlags = restored;
  ASSERT_EQ(progress.setFlags(0, tinta::core::item_flag::kStarred, 6, 300), tinta::core::ProgressStore::Status::Stored);
  ASSERT_TRUE(progress.load(0, restored));
  const auto flags = tinta::core::JournalEntry::control(body.uid, tinta::core::JournalEntry::kSetFlags,
                                                        tinta::core::item_flag::kStarred, 6, 300);
  ASSERT_TRUE(owner.acknowledge(event.identity, flags, beforeFlags, restored, 0));
  const auto published = state.files;
  EXPECT_FALSE(owner.acknowledge(reviewIdentity, entry, result.before, result.after, 1234));
  EXPECT_EQ(state.files, published);
  ASSERT_TRUE(owner.recoverUndo(recoveredUndo));
  EXPECT_EQ(recoveredUndo, EventIdentity{});
  event.ancestors[0] = event.identity;
  event.identity.sequence = 4;
  event.kind = EventKind::Review;
  event.studyDay = 7;
  event.schedulerVersion = 1;
  ASSERT_TRUE(storage.digest(configuration, event.schedulerConfiguration));
  body.kind = EventKind::Review;
  length = encodeTintaBody(body, bytes);
  ASSERT_TRUE(storage.digest(std::span(bytes).first(length), event.bodyHash));
  ASSERT_EQ(journal.append(event, std::span(bytes).first(length)), TintaJournalResult::Ok);
  const auto next = progress.review(0, tinta::core::Grade::Good, 2, 1234, 7, 400);
  ASSERT_EQ(next.status, tinta::core::ProgressStore::Status::Stored);
  const auto nextEntry = tinta::core::JournalEntry::review(body.uid, tinta::core::Grade::Good, 2, 1234, 7, 400);
  ASSERT_TRUE(owner.acknowledge(event.identity, nextEntry, next.before, next.after, 1234));
  ASSERT_TRUE(owner.recoverUndo(recoveredUndo));
  const auto firstMatch = recoveredUndo;
  event.ancestors[0] = event.identity;
  event.identity.sequence = 5;
  ASSERT_EQ(journal.append(event, std::span(bytes).first(length)), TintaJournalResult::Ok);
  ASSERT_TRUE(owner.acknowledge(event.identity, nextEntry, next.before, next.after, 1234));
  EXPECT_FALSE(owner.recoverUndo(recoveredUndo));
  EXPECT_EQ(recoveredUndo, firstMatch);
}

TEST(HalTintaJournalStorageTest, IncrementalRecoveryReplaysAuthorityAndPublishesBeforeNativePreparation) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  Identity course{}, generation{}, snapshot{};
  Digest pack{};
  course.fill(4);
  generation.fill(5);
  snapshot.fill(6);
  pack.fill(7);
  class Catalog final : public TintaSubjectCatalog {
    TintaSubjectMembership contains(EventKind, uint32_t uid) override {
      return uid == 1 ? TintaSubjectMembership::Present : TintaSubjectMembership::Missing;
    }
  } catalog;
  HalTintaJournalStorage storage;
  std::array<uint8_t, 1024> journalScratch{};
  TintaJournal journal(storage, journalScratch);
  ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
  TintaBody body;
  body.course = course;
  body.uid = 1;
  body.grade = 3;
  SyncEvent event;
  event.identity.origin.fill(1);
  event.identity.epoch = 1;
  event.identity.sequence = 1;
  event.storageGeneration = generation;
  event.resource = pack;
  event.kind = EventKind::Review;
  event.studyDay = 5;
  event.schedulerVersion = 1;
  std::array<uint8_t, MAX_TINTA_BODY_SIZE> bytes{};
  std::array<uint8_t, 6> configuration{};
  auto length = encodeTintaBody(body, bytes);
  ASSERT_TRUE(storage.digest(std::span(bytes).first(length), event.bodyHash));
  ASSERT_EQ(encodeTintaConfiguration(body.configuration, configuration), configuration.size());
  ASSERT_TRUE(storage.digest(configuration, event.schedulerConfiguration));
  ASSERT_EQ(journal.append(event, std::span(bytes).first(length)), TintaJournalResult::Ok);
  std::array<uint8_t, 512> scratch{};
  std::array<uint8_t, TINTA_DERIVED_MANIFEST_SIZE> baseline{};
  HalTintaReplaySession replay;
  ASSERT_TRUE(replay.run(course, catalog));
  HalTintaReplayExport output;
  ASSERT_TRUE(output.run(*replay.workingStore(), course, 5, scratch));
  ASSERT_TRUE(output.manifest(generation, pack, *replay.journalFrontier(), snapshot, 1, baseline));
  ASSERT_TRUE(replay.workingStore()->close());
  ASSERT_EQ(publishProvenTintaDerived(course, generation, pack, catalog, baseline, scratch),
            TintaPublicationResult::Ok);
  HalTintaAuthorityRetention authority;
  ASSERT_TRUE(authority.establish(baseline, course, generation, pack, catalog));
  ASSERT_TRUE(authority.prove(baseline, course, generation, pack, catalog, *replay.journalFrontier()));
  bool retained = true;
  const auto retentionProof = [](void* context, const TintaDerivedManifestView&, const Digest&) {
    return *static_cast<bool*>(context);
  };
  HalTintaIncrementalRecovery recovery(course, &retained, retentionProof);
  std::array<char, COURSE_STATE_PATH_SIZE> intentPath{};
  ASSERT_TRUE(tintaDerivedRecordPath(course, TintaDerivedRecord::Intent, intentPath));
  state.files[intentPath.data()] = {baseline.begin(), baseline.end()};
  const auto pending = state.files;
  EXPECT_EQ(recovery.run(generation, pack, catalog, 5, snapshot), TintaIncrementalRecoveryResult::Failed);
  EXPECT_EQ(state.files, pending);
  state.files.erase(intentPath.data());
  auto foreign = generation;
  foreign[0] ^= 1;
  EXPECT_EQ(recovery.run(foreign, pack, catalog, 5, snapshot), TintaIncrementalRecoveryResult::Failed);
  ASSERT_EQ(recovery.run(generation, pack, catalog, 5, snapshot), TintaIncrementalRecoveryResult::Unchanged);
  event.ancestors[0] = event.identity;
  event.ancestorCount = 1;
  event.identity.sequence = 2;
  event.kind = EventKind::Star;
  event.studyDay = 6;
  event.schedulerVersion = 0;
  event.schedulerConfiguration = {};
  body.kind = EventKind::Star;
  body.enabled = true;
  length = encodeTintaBody(body, bytes);
  ASSERT_TRUE(storage.digest(std::span(bytes).first(length), event.bodyHash));
  ASSERT_EQ(journal.append(event, std::span(bytes).first(length)), TintaJournalResult::Ok);
  HalJournalCausalAuditSession retentionAudit;
  Digest advanced{};
  ASSERT_TRUE(retentionAudit.run(&advanced, &course, &catalog));
  ASSERT_TRUE(authority.prove(baseline, course, generation, pack, catalog, advanced));
  Digest checkpointDigest{};
  ASSERT_TRUE(storage.digest(baseline, checkpointDigest));
  std::array<char, TINTA_CHECKPOINT_PATH_SIZE> checkpointPath{};
  ASSERT_TRUE(tintaAuthorityCheckpointPath(checkpointDigest, false, checkpointPath));
  const auto savedCheckpoint = state.files.at(checkpointPath.data());
  state.files.erase(checkpointPath.data());
  Identity nextSnapshot{};
  nextSnapshot.fill(8);
  EXPECT_EQ(recovery.run(generation, pack, catalog, 5, nextSnapshot), TintaIncrementalRecoveryResult::Failed);
  EXPECT_EQ(recovery.journalFrontier(), nullptr);
  EXPECT_EQ(state.files.count(checkpointPath.data()), 0U);
  state.files[checkpointPath.data()] = savedCheckpoint;
  const auto retainedFiles = state.files;
  ASSERT_TRUE(storage.close());
  state.files.erase(TINTA_JOURNAL_EVENTS);
  state.files.erase(TINTA_JOURNAL_HEADER_A);
  state.files.erase(TINTA_JOURNAL_HEADER_B);
  {
    HalTintaJournalStorage replacementStorage;
    std::array<uint8_t, 1024> replacementScratch{};
    TintaJournal replacement(replacementStorage, replacementScratch);
    ASSERT_EQ(replacement.open(), TintaJournalResult::Ok);
    HalJournalCausalAuditSession replacementAudit;
    Digest replacementFrontier{};
    ASSERT_TRUE(replacementAudit.run(&replacementFrontier, &course, &catalog));
    EXPECT_FALSE(authority.prove(baseline, course, generation, pack, catalog, replacementFrontier));
    auto replacementEvent = event;
    replacementEvent.identity.sequence = 1;
    replacementEvent.ancestorCount = 0;
    replacementEvent.ancestors = {};
    replacementEvent.kind = EventKind::Star;
    TintaBody replacementBody = body;
    replacementBody.enabled = false;
    std::array<uint8_t, MAX_TINTA_BODY_SIZE> replacementBytes{};
    const auto replacementLength = encodeTintaBody(replacementBody, replacementBytes);
    ASSERT_TRUE(
        replacementStorage.digest(std::span(replacementBytes).first(replacementLength), replacementEvent.bodyHash));
    ASSERT_EQ(replacement.append(replacementEvent, std::span(replacementBytes).first(replacementLength)),
              TintaJournalResult::Ok);
    ASSERT_TRUE(replacementAudit.run(&replacementFrontier, &course, &catalog));
    EXPECT_FALSE(authority.prove(baseline, course, generation, pack, catalog, replacementFrontier));
    EXPECT_FALSE(authority.establish(baseline, course, generation, pack, catalog));
  }
  state.files = retainedFiles;
  ASSERT_TRUE(authority.prove(baseline, course, generation, pack, catalog, advanced));
  EXPECT_FALSE(authority.prove(baseline, course, generation, pack, catalog, *replay.journalFrontier()));
  EXPECT_FALSE(authority.establish(baseline, course, generation, pack, catalog));
  state.readErrorPath = TINTA_JOURNAL_EVENTS;
  EXPECT_FALSE(authority.prove(baseline, course, generation, pack, catalog, advanced));
  state.readErrorPath.clear();
  EXPECT_EQ(recovery.run(generation, pack, catalog, 5, snapshot), TintaIncrementalRecoveryResult::Failed);
  snapshot.fill(8);
  retained = false;
  const auto beforeRetention = state.files;
  EXPECT_EQ(recovery.run(generation, pack, catalog, 5, snapshot), TintaIncrementalRecoveryResult::Failed);
  std::array<char, COURSE_STATE_PATH_SIZE> receiptPath{};
  ASSERT_TRUE(tintaDerivedRecordPath(course, TintaDerivedRecord::Receipt, receiptPath));
  EXPECT_EQ(state.files[receiptPath.data()], beforeRetention.at(receiptPath.data()));
  retained = true;
  std::array<char, COURSE_STATE_PATH_SIZE> activePath{}, candidatePath{};
  ASSERT_TRUE(tintaDerivedFilePath(course, TintaDerivedFile::Items, TintaDerivedRole::Active, activePath));
  ASSERT_TRUE(tintaDerivedFilePath(course, TintaDerivedFile::Items, TintaDerivedRole::Candidate, candidatePath));
  const auto originalItems = state.files[activePath.data()];
  state.failSyncPath = candidatePath.data();
  EXPECT_EQ(recovery.run(generation, pack, catalog, 5, snapshot), TintaIncrementalRecoveryResult::Failed);
  EXPECT_EQ(state.files[activePath.data()], originalItems);
  EXPECT_EQ(recovery.journalFrontier(), nullptr);
  state.failSyncPath.clear();
  ASSERT_EQ(recovery.run(generation, pack, catalog, 5, snapshot), TintaIncrementalRecoveryResult::Rebuilt);
  ASSERT_NE(recovery.journalFrontier(), nullptr);
  std::array<uint8_t, TINTA_DERIVED_MANIFEST_SIZE> receipt{};
  HalTintaDerivedRecordReader reader(course, scratch);
  ASSERT_EQ(reader.load(TintaDerivedRecord::Receipt, receipt), TintaDerivedRecordLoad::Loaded);
  TintaDerivedManifestView manifest;
  ASSERT_TRUE(manifest.decode(receipt));
  EXPECT_EQ(manifest.revision(), 2U);
  EXPECT_EQ(manifest.studyDay(), 6U);
  EXPECT_TRUE(manifest.matches(course, generation, pack, *recovery.journalFrontier()));
  std::array<char, COURSE_STATE_PATH_SIZE> path{};
  ASSERT_TRUE(tintaDerivedFilePath(course, TintaDerivedFile::Items, TintaDerivedRole::Active, path));
  tinta::core::ItemState item;
  ASSERT_TRUE(tinta::core::ItemState::decode(state.files[path.data()].data() + 1024, item));
  EXPECT_NE(item.flags & tinta::core::item_flag::kStarred, 0U);
  const auto active = state.files[path.data()];
  ASSERT_EQ(recovery.run(generation, pack, catalog, 5, snapshot), TintaIncrementalRecoveryResult::Unchanged);
  EXPECT_EQ(state.files[path.data()], active);
}

TEST(HalTintaJournalStorageTest, PrefixFrontierRetainsOldEventsWhenNewOriginsSortBeforeThem) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  HalTintaJournalStorage storage;
  std::array<uint8_t, 1024> scratch{};
  TintaJournal journal(storage, scratch);
  ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
  TintaBody body;
  body.course.fill(4);
  body.uid = 1;
  body.kind = EventKind::Star;
  body.enabled = true;
  std::array<uint8_t, MAX_TINTA_BODY_SIZE> bytes{};
  const auto length = encodeTintaBody(body, bytes);
  SyncEvent event;
  event.identity.origin.fill(9);
  event.identity.epoch = 1;
  event.identity.sequence = 1;
  event.storageGeneration.fill(5);
  event.resource.fill(6);
  event.kind = EventKind::Star;
  event.studyDay = 5;
  ASSERT_TRUE(storage.digest(std::span(bytes).first(length), event.bodyHash));
  ASSERT_EQ(journal.append(event, std::span(bytes).first(length)), TintaJournalResult::Ok);
  HalJournalCausalAuditSession audit;
  Digest original{}, prefix{}, full{};
  EXPECT_FALSE(audit.prefixFrontier(1, prefix));
  ASSERT_TRUE(audit.run(&original));
  ASSERT_TRUE(audit.prefixFrontier(1, prefix));
  EXPECT_EQ(prefix, original);
  event.identity.origin.fill(1);
  ASSERT_EQ(journal.append(event, std::span(bytes).first(length)), TintaJournalResult::Ok);
  ASSERT_TRUE(audit.run(&full));
  EXPECT_NE(full, original);
  ASSERT_TRUE(audit.prefixFrontier(1, prefix));
  EXPECT_EQ(prefix, original);
  ASSERT_TRUE(audit.prefixFrontier(2, prefix));
  EXPECT_EQ(prefix, full);
  prefix.fill(7);
  const auto unchanged = prefix;
  EXPECT_FALSE(audit.prefixFrontier(3, prefix));
  EXPECT_EQ(prefix, unchanged);
  ASSERT_TRUE(audit.prefixFrontier(0, prefix));
  EXPECT_NE(prefix, original);
  state.readErrorPath = TINTA_JOURNAL_EVENTS;
  prefix = unchanged;
  EXPECT_FALSE(audit.prefixFrontier(1, prefix));
  EXPECT_EQ(prefix, unchanged);
}

TEST(HalTintaJournalStorageTest, AuthorityCheckpointPublicationPreservesConflictsAndRecoversRenameReply) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  constexpr auto path = "/.crosspoint/companion/checkpoint-test";
  constexpr auto stage = "/.crosspoint/companion/checkpoint-test-next";
  TintaAuthorityCheckpoint value;
  value.manifest.fill(1);
  value.frontier.fill(2);
  value.count = 3;
  HalTintaAuthorityCheckpointStore records(path, stage);
  TintaAuthorityCheckpoint output = value;
  EXPECT_EQ(records.load(output), TintaAuthorityCheckpointResult::Missing);
  EXPECT_EQ(output, value);
  state.failSync = true;
  EXPECT_EQ(records.persist(value), TintaAuthorityCheckpointResult::IoError);
  EXPECT_EQ(state.files.count(path), 0U);
  state.failSync = false;
  state.failRenameAfterSource = stage;
  EXPECT_EQ(records.persist(value), TintaAuthorityCheckpointResult::Ok);
  ASSERT_EQ(records.load(output), TintaAuthorityCheckpointResult::Ok);
  EXPECT_EQ(output, value);
  const auto identical = state.files;
  const auto renames = state.renames;
  EXPECT_EQ(records.persist(value), TintaAuthorityCheckpointResult::Ok);
  EXPECT_EQ(state.files, identical);
  EXPECT_EQ(state.renames, renames);
  value.count++;
  EXPECT_EQ(records.persist(value), TintaAuthorityCheckpointResult::Conflict);
  EXPECT_EQ(state.files, identical);
  state.files[path][10] ^= 1;
  const auto corrupt = state.files;
  EXPECT_EQ(records.persist(value), TintaAuthorityCheckpointResult::Corrupt);
  EXPECT_EQ(state.files, corrupt);
  EXPECT_EQ(records.load(output), TintaAuthorityCheckpointResult::Corrupt);
  EXPECT_NE(output, value);
}

TEST(HalTintaJournalStorageTest, AuthorityCheckpointsSeparateManifestsAndRejectMisaddressedRecords) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  HalTintaAuthorityCheckpoints records;
  TintaAuthorityCheckpoint first;
  first.manifest.fill(1);
  first.frontier.fill(2);
  first.count = 3;
  auto second = first;
  second.manifest.fill(4);
  second.count = 5;
  ASSERT_EQ(records.persist(first), TintaAuthorityCheckpointResult::Ok);
  ASSERT_EQ(records.persist(second), TintaAuthorityCheckpointResult::Ok);
  EXPECT_EQ(state.files.size(), 2U);
  TintaAuthorityCheckpoint output;
  ASSERT_EQ(records.load(first.manifest, output), TintaAuthorityCheckpointResult::Ok);
  EXPECT_EQ(output, first);
  ASSERT_EQ(records.load(second.manifest, output), TintaAuthorityCheckpointResult::Ok);
  EXPECT_EQ(output, second);
  Digest missing{};
  missing.fill(9);
  EXPECT_EQ(records.load(missing, output), TintaAuthorityCheckpointResult::Missing);
  EXPECT_EQ(output, second);
  std::array<char, TINTA_CHECKPOINT_PATH_SIZE> path{};
  ASSERT_TRUE(tintaAuthorityCheckpointPath(first.manifest, false, path));
  EXPECT_EQ(std::string(path.data()),
            "/.crosspoint/companion/tinta-checkpoint-0101010101010101010101010101010101010101010101010101010101010101");
  std::array<uint8_t, TINTA_AUTHORITY_CHECKPOINT_SIZE> bytes{};
  ASSERT_TRUE(encodeTintaAuthorityCheckpoint(second, bytes));
  state.files[path.data()] = {bytes.begin(), bytes.end()};
  const auto before = state.files;
  EXPECT_EQ(records.load(first.manifest, output), TintaAuthorityCheckpointResult::Conflict);
  EXPECT_EQ(output, second);
  EXPECT_EQ(records.persist(first), TintaAuthorityCheckpointResult::Conflict);
  EXPECT_EQ(state.files, before);
  Digest invalid{};
  EXPECT_EQ(records.load(invalid, output), TintaAuthorityCheckpointResult::Invalid);
  EXPECT_EQ(output, second);
}

TEST(HalTintaJournalStorageTest, PreferenceResolutionUsesAuditedIndexAndClearsFailedOutput) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  HalTintaJournalStorage storage;
  std::array<uint8_t, 1024> scratch{};
  TintaJournal journal(storage, scratch);
  ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
  std::array<uint8_t, 8> body{1, 4, 32, 1, 20, 0, 0, 0};
  SyncEvent event;
  event.identity.origin.fill(1);
  event.identity.epoch = 1;
  event.identity.sequence = 1;
  event.storageGeneration.fill(2);
  event.kind = EventKind::Preference;
  event.resource = PREFERENCE_SCOPE;
  ASSERT_TRUE(storage.digest(body, event.bodyHash));
  ASSERT_EQ(journal.append(event, body), TintaJournalResult::Ok);
  HalJournalCausalAuditSession audit;
  TintaPreferenceResolution resolution;
  EXPECT_EQ(audit.resolveTintaPreferences(resolution), TintaJournalResult::Unavailable);
  ASSERT_TRUE(audit.run());
  ASSERT_EQ(audit.resolveTintaPreferences(resolution), TintaJournalResult::Ok);
  ASSERT_EQ(resolution.bodies().size(), 1U);
  EXPECT_EQ(resolution.bodies()[0][4], 20);
  tinta_test::MemStore native;
  tinta::core::Profile profile;
  profile.fullRefreshEvery = 17;
  native.failFrom(0);
  EXPECT_FALSE(persistResolvedTintaPreferences(native, profile, resolution.bodies()));
  EXPECT_EQ(profile.newPerDay, 10);
  native.powerOn();
  ASSERT_TRUE(persistResolvedTintaPreferences(native, profile, resolution.bodies()));
  EXPECT_EQ(profile.newPerDay, 20);
  EXPECT_EQ(profile.fullRefreshEvery, 17);
  tinta::core::Profile loaded;
  ASSERT_EQ(loaded.load(native), tinta::core::Profile::LoadResult::Loaded);
  EXPECT_EQ(loaded.newPerDay, 20);
  EXPECT_EQ(loaded.fullRefreshEvery, 17);
  const auto calls = native.calls;
  EXPECT_TRUE(persistResolvedTintaPreferences(native, profile, resolution.bodies()));
  EXPECT_EQ(native.calls, calls);
  EXPECT_EQ(audit.resolveTintaPreferences(resolution), TintaJournalResult::Unavailable);
  EXPECT_TRUE(resolution.bodies().empty());
  event.identity.origin.fill(3);
  body[4] = 30;
  ASSERT_TRUE(storage.digest(body, event.bodyHash));
  ASSERT_EQ(journal.append(event, body), TintaJournalResult::Ok);
  ASSERT_TRUE(audit.run());
  EXPECT_EQ(audit.resolveTintaPreferences(resolution), TintaJournalResult::Conflict);
  EXPECT_EQ(resolution.conflictMask(), 1);
  EXPECT_TRUE(resolution.bodies().empty());
  ASSERT_TRUE(audit.run());
  state.readErrorPath = TINTA_JOURNAL_EVENTS;
  EXPECT_EQ(audit.resolveTintaPreferences(resolution), TintaJournalResult::IoError);
  EXPECT_EQ(resolution.conflictMask(), 0);
  EXPECT_TRUE(resolution.bodies().empty());
}

TEST(HalTintaJournalStorageTest, LegacyAdmissionRequiresNoLearningAuthorityForTheCourse) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
  Identity course{}, other{};
  course.fill(4);
  other.fill(5);
  const auto absent = state.files;
  ASSERT_TRUE(allowLegacyTintaWithoutReceipt(course));
  EXPECT_EQ(state.files, absent);
  HalTintaJournalStorage storage;
  std::array<uint8_t, 1024> scratch{};
  TintaJournal journal(storage, scratch);
  ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
  ASSERT_TRUE(allowLegacyTintaWithoutReceipt(course));
  TintaBody body;
  body.kind = EventKind::Star;
  body.course = course;
  body.uid = 1;
  body.enabled = true;
  std::array<uint8_t, MAX_TINTA_BODY_SIZE> encoded{};
  const auto length = encodeTintaBody(body, encoded);
  SyncEvent event;
  event.identity.origin.fill(1);
  event.identity.epoch = event.identity.sequence = 1;
  event.storageGeneration.fill(2);
  event.kind = body.kind;
  event.resource.fill(3);
  ASSERT_TRUE(storage.digest(std::span(encoded).first(length), event.bodyHash));
  ASSERT_EQ(journal.append(event, std::span(encoded).first(length)), TintaJournalResult::Ok);
  ASSERT_TRUE(storage.close());
  EXPECT_FALSE(allowLegacyTintaWithoutReceipt(course));
  EXPECT_TRUE(allowLegacyTintaWithoutReceipt(other));
  HalJournalCausalAuditSession audit;
  bool output = true;
  EXPECT_FALSE(audit.containsTintaCourse(other, output));
  EXPECT_TRUE(output);
  ASSERT_TRUE(audit.run());
  state.readErrorPath = TINTA_JOURNAL_EVENTS;
  EXPECT_FALSE(audit.containsTintaCourse(other, output));
  EXPECT_TRUE(output);
  EXPECT_FALSE(allowLegacyTintaWithoutReceipt(other));
  state.readErrorPath.clear();
  ASSERT_TRUE(audit.run());
  output = false;
  state.failClosePath = TINTA_JOURNAL_EVENTS;
  EXPECT_FALSE(audit.containsTintaCourse(course, output));
  EXPECT_FALSE(output);
  state.failClosePath.clear();
  EXPECT_FALSE(allowLegacyTintaWithoutReceipt(course));
  state.files[TINTA_JOURNAL_EVENTS][40] ^= 1;
  EXPECT_FALSE(allowLegacyTintaWithoutReceipt(other));
}

TEST(HalTintaJournalStorageTest, MigrationAdmissionPublicationRecoversAndPreservesForeignRecords) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  std::string fixturePath = TINTA_FRONTIER_FIXTURE;
  fixturePath.resize(fixturePath.find_last_of('/') + 1);
  std::ifstream fixture(fixturePath + "TintaMigrationAdmission-v1.fixture", std::ios::binary);
  const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(fixture), {}};
  TintaMigrationAdmission value;
  ASSERT_TRUE(decodeTintaMigrationAdmission(bytes, value));
  static constexpr auto PATH = "/.crosspoint/companion/migration-admission";
  static constexpr auto STAGE = "/.crosspoint/companion/migration-admission-next";
  HalTintaMigrationAdmissionStore records(PATH, STAGE);
  TintaMigrationAdmission output = value;
  EXPECT_EQ(records.load(output), TintaMigrationAdmissionResult::Missing);
  state.failSync = true;
  EXPECT_EQ(records.persist(value), TintaMigrationAdmissionResult::IoError);
  EXPECT_FALSE(state.files.contains(PATH));
  state.failSync = false;
  state.failRenameAfterSource = STAGE;
  ASSERT_EQ(records.persist(value), TintaMigrationAdmissionResult::Ok);
  ASSERT_EQ(records.load(output), TintaMigrationAdmissionResult::Ok);
  EXPECT_EQ(output, value);
  const auto published = state.files;
  const auto renames = state.renames;
  EXPECT_EQ(records.persist(value), TintaMigrationAdmissionResult::Ok);
  EXPECT_EQ(state.files, published);
  EXPECT_EQ(state.renames, renames);
  auto foreign = value;
  foreign.backupTransaction[0] ^= 1;
  EXPECT_EQ(records.persist(foreign), TintaMigrationAdmissionResult::Conflict);
  EXPECT_EQ(state.files, published);
  state.files[PATH][40] ^= 1;
  const auto corrupt = state.files;
  EXPECT_EQ(records.load(output), TintaMigrationAdmissionResult::Corrupt);
  EXPECT_EQ(output, value);
  EXPECT_EQ(records.persist(value), TintaMigrationAdmissionResult::Corrupt);
  EXPECT_EQ(state.files, corrupt);
  state.files = published;
  state.files[STAGE] = state.files[PATH];
  state.files.erase(PATH);
  std::array<uint8_t, TINTA_MIGRATION_ADMISSION_SIZE> encoded{};
  ASSERT_TRUE(encodeTintaMigrationAdmission(foreign, encoded));
  state.files[STAGE] = {encoded.begin(), encoded.end()};
  const auto stagedForeign = state.files;
  EXPECT_EQ(records.persist(value), TintaMigrationAdmissionResult::Conflict);
  EXPECT_EQ(state.files, stagedForeign);
  state.files[STAGE] = bytes;
  state.failClosePath = STAGE;
  EXPECT_EQ(records.persist(value), TintaMigrationAdmissionResult::IoError);
  EXPECT_FALSE(state.files.contains(PATH));
  state.failClosePath.clear();
  state.failRenameAfterSource.clear();
  HalTintaMigrationAdmissionStore recovered(PATH, STAGE);
  ASSERT_EQ(recovered.persist(value), TintaMigrationAdmissionResult::Ok);
  ASSERT_EQ(recovered.load(output), TintaMigrationAdmissionResult::Ok);
  EXPECT_EQ(output, value);
}

TEST(HalTintaJournalStorageTest, MigrationSourceProofRejectsNewOptionalFilesAfterCapture) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  Identity reader{}, generation{}, course{}, transaction{};
  reader.fill(1);
  generation.fill(2);
  course.fill(3);
  transaction.fill(4);
  std::array<char, COURSE_STATE_DIRECTORY_SIZE> root{};
  ASSERT_TRUE(courseStateDirectory(course, root));
  ASSERT_TRUE(Storage.ensureDirectoryExists(root.data()));
  for (const auto* name : {"reviews.log", "items.bin", "profile.bin"})
    state.files[std::string(root.data()) + "/" + name] = std::vector<uint8_t>(80, 5);
  auto session = makeUniqueNoThrow<HalLegacyTintaBackupSession>();
  ASSERT_TRUE(session);
  ASSERT_TRUE(session->captureCourse(reader, generation, course, transaction, true));
  ASSERT_TRUE(session->matchesCurrentCourse());
  session.reset();
  session = makeUniqueNoThrow<HalLegacyTintaBackupSession>();
  ASSERT_TRUE(session);
  ASSERT_TRUE(session->prepareCourse(reader, generation, course, transaction, true, true));
  ASSERT_TRUE(session->recover());
  ASSERT_TRUE(session->matchesCurrentCourse());
  for (const auto* name : {"read.bin", "starred.bin", "days.bin", "session.bin"}) {
    const auto path = std::string(root.data()) + "/" + name;
    state.files[path] = {};
    const auto before = state.files;
    EXPECT_FALSE(session->matchesCurrentCourse());
    EXPECT_EQ(state.files, before);
    ASSERT_TRUE(session->verify());
    state.files.erase(path);
    EXPECT_TRUE(session->matchesCurrentCourse());
  }
}

TEST(HalTintaJournalStorageTest, MigrationBackupProofBindsExactReaderGenerationCourseTransactionAndManifest) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  TintaMigrationAdmission admission;
  admission.reader.fill(1);
  admission.merge.generation.fill(2);
  admission.course.fill(3);
  admission.backupTransaction.fill(4);
  admission.merge.owner.fill(5);
  admission.merge.transaction.fill(6);
  admission.merge.previous = {0, 512, {}};
  admission.merge.previous.frontier.fill(7);
  admission.merge.merged = {1, 1024, {}};
  admission.merge.merged.frontier.fill(8);
  admission.resource.fill(9);
  std::array<char, COURSE_STATE_DIRECTORY_SIZE> root{};
  ASSERT_TRUE(courseStateDirectory(admission.course, root));
  ASSERT_TRUE(Storage.ensureDirectoryExists(root.data()));
  for (const auto* name : {"reviews.log", "items.bin", "profile.bin"})
    state.files[std::string(root.data()) + "/" + name] = std::vector<uint8_t>(80, 5);
  auto backup = makeUniqueNoThrow<HalLegacyTintaBackupSession>();
  ASSERT_TRUE(backup);
  ASSERT_TRUE(backup->captureCourse(admission.reader, admission.merge.generation, admission.course,
                                    admission.backupTransaction, true));
  std::array<uint8_t, LEGACY_TINTA_BACKUP_MANIFEST_SIZE> manifest{};
  ASSERT_TRUE(backup->readManifest(manifest));
  HalTintaJournalStorage hashing;
  ASSERT_TRUE(hashing.digest(manifest, admission.backupManifest));
  const auto unchanged = state.files;
  ASSERT_TRUE(backup->verifyMigrationBackup(admission, hashing));
  EXPECT_EQ(state.files, unchanged);
  for (const auto field : {0, 1, 2, 3, 4}) {
    auto foreign = admission;
    if (field == 0) foreign.reader[0] ^= 1;
    if (field == 1) foreign.merge.generation[0] ^= 1;
    if (field == 2) foreign.course[0] ^= 1;
    if (field == 3) foreign.backupTransaction[0] ^= 1;
    if (field == 4) foreign.backupManifest[0] ^= 1;
    EXPECT_FALSE(backup->verifyMigrationBackup(foreign, hashing));
    EXPECT_EQ(state.files, unchanged);
  }
  state.files[std::string(root.data()) + "/profile.bin"][3] ^= 1;
  const auto edited = state.files;
  EXPECT_FALSE(backup->verifyMigrationBackup(admission, hashing));
  EXPECT_EQ(state.files, edited);
  EXPECT_TRUE(backup->verify());
}

TEST(HalTintaJournalStorageTest, AddressedMigrationAdmissionsSeparateTransactionsAndRejectWrongOwnership) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  std::string fixturePath = TINTA_FRONTIER_FIXTURE;
  fixturePath.resize(fixturePath.find_last_of('/') + 1);
  std::ifstream fixture(fixturePath + "TintaMigrationAdmission-v1.fixture", std::ios::binary);
  const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(fixture), {}};
  TintaMigrationAdmission first;
  ASSERT_TRUE(decodeTintaMigrationAdmission(bytes, first));
  auto second = first;
  second.merge.transaction[0] ^= 1;
  HalTintaJournalStorage hashing;
  HalTintaMigrationAdmissions records(hashing);
  TintaMigrationAdmission output = second;
  EXPECT_EQ(records.load(first.course, first.merge, output), TintaMigrationAdmissionResult::Missing);
  EXPECT_EQ(output, second);
  ASSERT_EQ(records.persist(first), TintaMigrationAdmissionResult::Ok);
  ASSERT_EQ(records.persist(second), TintaMigrationAdmissionResult::Ok);
  ASSERT_EQ(records.load(first.course, first.merge, output), TintaMigrationAdmissionResult::Ok);
  EXPECT_EQ(output, first);
  ASSERT_EQ(records.load(second.course, second.merge, output), TintaMigrationAdmissionResult::Ok);
  EXPECT_EQ(output, second);
  std::array<uint8_t, TINTA_MIGRATION_ADDRESS_SIZE> address{};
  ASSERT_TRUE(encodeTintaMigrationAddress(first.course, first.merge, address));
  EXPECT_TRUE(std::equal(first.course.begin(), first.course.end(), address.begin()));
  EXPECT_TRUE(std::equal(bytes.begin() + 4, bytes.begin() + 140, address.begin() + 16));
  Digest digest{};
  ASSERT_TRUE(hashing.digest(address, digest));
  std::array<char, TINTA_MIGRATION_PATH_SIZE> path{};
  ASSERT_TRUE(tintaMigrationAdmissionPath(digest, false, path));
  EXPECT_EQ(std::string(path.data()),
            "/.crosspoint/companion/tinta-migration-"
            "b39edcb4073e57a13c2ec71cee741f1404e0338593cd53885464c63d49a7c642");
  std::array<uint8_t, TINTA_MIGRATION_ADMISSION_SIZE> foreign{};
  ASSERT_TRUE(encodeTintaMigrationAdmission(second, foreign));
  state.files[path.data()] = {foreign.begin(), foreign.end()};
  const auto unchanged = state.files;
  EXPECT_EQ(records.load(first.course, first.merge, output), TintaMigrationAdmissionResult::Conflict);
  EXPECT_EQ(output, second);
  EXPECT_EQ(state.files, unchanged);
  EXPECT_EQ(records.persist(first), TintaMigrationAdmissionResult::Conflict);
  EXPECT_EQ(state.files, unchanged);
  auto invalid = first.merge;
  invalid.merged = invalid.previous;
  invalid.merged.recordSize = 1024;
  EXPECT_EQ(records.load(first.course, invalid, output), TintaMigrationAdmissionResult::Invalid);
  EXPECT_EQ(output, second);
  auto small = path;
  EXPECT_FALSE(tintaMigrationAdmissionPath(digest, true, std::span(small).first(10)));
  EXPECT_EQ(small, path);
}

TEST(HalTintaJournalStorageTest, ActiveJournalStateAuditsCurrentExtentInsteadOfBackupFormats) {
  std::string fixturePath = TINTA_FRONTIER_FIXTURE;
  fixturePath.erase(fixturePath.find_last_of('/') + 1);
  fixturePath += "JournalStateReply-v1.fixture";
  std::ifstream fixture(fixturePath, std::ios::binary);
  std::array<uint8_t, JOURNAL_STATE_REPLY_SIZE> expected{};
  fixture.read(reinterpret_cast<char*>(expected.data()), expected.size());
  ASSERT_EQ(fixture.gcount(), expected.size());
  for (const uint16_t stride : {512, 1024}) {
    auto& state = inventory_hal_test::state;
    state = {};
    state.enumerateFileMap = true;
    ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
    std::array<uint8_t, 1024> scratch{};
    {
      HalTintaJournalStorage storage;
      TintaJournal journal(storage, std::span(scratch).first(stride));
      ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
    }
    {
      HalTintaJournalStorage backup(TintaJournalLocation::MergeBackup);
      TintaJournal journal(backup, std::span(scratch).first(stride == 512 ? 1024 : 512));
      ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
    }
    Identity generation{};
    generation.fill(2);
    std::array<uint8_t, JOURNAL_STATE_REQUEST_SIZE> request{'J', 'S', 'T', 1};
    std::copy(generation.begin(), generation.end(), request.begin() + 4);
    std::array<uint8_t, JOURNAL_STATE_REPLY_SIZE> reply{};
    ASSERT_EQ(handleJournalStateQuery(generation, request, reply), reply.size());
    EXPECT_EQ(tinta_body_detail::read(reply, 4, 4), 0u);
    EXPECT_EQ(tinta_body_detail::read(reply, 8, 2), stride);
    EXPECT_TRUE(std::equal(reply.begin() + 12, reply.end(), expected.begin() + 12));
    {
      HalTintaJournalStorage storage;
      TintaJournal journal(storage, std::span(scratch).first(stride));
      ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
      TintaBody body;
      body.kind = EventKind::Star;
      body.course.fill(3);
      body.uid = 1;
      body.enabled = true;
      std::array<uint8_t, MAX_TINTA_BODY_SIZE> encoded{};
      const auto length = encodeTintaBody(body, encoded);
      SyncEvent event;
      event.identity = {generation, 1, 1};
      event.storageGeneration = generation;
      event.resource.fill(4);
      event.kind = body.kind;
      ASSERT_TRUE(storage.digest(std::span(encoded).first(length), event.bodyHash));
      ASSERT_EQ(journal.append(event, std::span(encoded).first(length)), TintaJournalResult::Ok);
    }
    ASSERT_EQ(handleJournalStateQuery(generation, request, reply), reply.size());
    EXPECT_EQ(tinta_body_detail::read(reply, 4, 4), 1u);
    EXPECT_EQ(tinta_body_detail::read(reply, 8, 2), stride);
    EXPECT_FALSE(std::equal(reply.begin() + 12, reply.end(), expected.begin() + 12));
    const auto unchanged = reply;
    auto foreign = generation;
    foreign[0] ^= 1;
    EXPECT_EQ(handleJournalStateQuery(foreign, request, reply), 0u);
    EXPECT_EQ(reply, unchanged);
    state.failRead = state.reads + 1;
    EXPECT_EQ(handleJournalStateQuery(generation, request, reply), 0u);
    EXPECT_EQ(reply, unchanged);
    state.failRead = 0;
    state.failClose = true;
    EXPECT_EQ(handleJournalStateQuery(generation, request, reply), 0u);
    EXPECT_EQ(reply, unchanged);
    state.failClose = false;
  }
}

TEST(HalTintaJournalStorageTest, PortableResolutionUsesAuditedIndexAndClearsFailedOutput) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  HalTintaJournalStorage storage;
  std::array<uint8_t, 1024> scratch{};
  TintaJournal journal(storage, scratch);
  ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
  std::array<uint8_t, 8> body{1, 4, 8, 1, 20, 0, 0, 0};
  SyncEvent event;
  event.identity.origin.fill(1);
  event.identity.epoch = 1;
  event.identity.sequence = 1;
  event.storageGeneration.fill(2);
  event.kind = EventKind::Preference;
  event.resource = PREFERENCE_SCOPE;
  ASSERT_TRUE(storage.digest(body, event.bodyHash));
  ASSERT_EQ(journal.append(event, body), TintaJournalResult::Ok);
  HalJournalCausalAuditSession audit;
  PortablePreferenceResolution resolution;
  EXPECT_EQ(audit.resolvePortablePreferences(resolution), TintaJournalResult::Unavailable);
  ASSERT_TRUE(audit.run());
  ASSERT_EQ(audit.resolvePortablePreferences(resolution), TintaJournalResult::Ok);
  ASSERT_EQ(resolution.bodies().size(), 1U);
  EXPECT_EQ(resolution.bodies()[0][4], 20);
  EXPECT_EQ(audit.resolvePortablePreferences(resolution), TintaJournalResult::Unavailable);
  EXPECT_TRUE(resolution.bodies().empty());
  event.identity.origin.fill(3);
  body[4] = 30;
  ASSERT_TRUE(storage.digest(body, event.bodyHash));
  ASSERT_EQ(journal.append(event, body), TintaJournalResult::Ok);
  ASSERT_TRUE(audit.run());
  EXPECT_EQ(audit.resolvePortablePreferences(resolution), TintaJournalResult::Conflict);
  EXPECT_EQ(resolution.conflictMask(), 1U << 7);
  EXPECT_TRUE(resolution.bodies().empty());
  ASSERT_TRUE(audit.run());
  state.readErrorPath = TINTA_JOURNAL_EVENTS;
  EXPECT_EQ(audit.resolvePortablePreferences(resolution), TintaJournalResult::IoError);
  EXPECT_EQ(resolution.conflictMask(), 0);
  EXPECT_TRUE(resolution.bodies().empty());
}

TEST(HalTintaJournalStorageTest, GenericOnlyCommitWithoutCourseSurvivesLostReply) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
  const std::array<uint8_t, 8> body{1, 4, 8, 1, 20, 0, 0, 0};
  SyncEvent event;
  event.identity.origin.fill(1);
  event.identity.epoch = event.identity.sequence = 1;
  event.storageGeneration.fill(2);
  event.kind = EventKind::Preference;
  event.resource = PREFERENCE_SCOPE;
  auto expected = mergeIntent();
  for (const auto location : {TintaJournalLocation::Active, TintaJournalLocation::MergeCandidate}) {
    HalTintaJournalStorage storage(location);
    std::array<uint8_t, 1024> scratch{};
    TintaJournal journal(storage, scratch);
    ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
    if (location == TintaJournalLocation::MergeCandidate) {
      ASSERT_TRUE(storage.digest(body, event.bodyHash));
      ASSERT_EQ(journal.append(event, body), TintaJournalResult::Ok);
    }
  }
  {
    HalJournalCausalAuditSession active, candidate(TintaJournalLocation::MergeCandidate);
    ASSERT_TRUE(active.run(&expected.previous.frontier));
    expected.previous.count = active.recordCount();
    expected.previous.recordSize = active.recordSize();
    ASSERT_TRUE(candidate.run(&expected.merged.frontier));
    expected.merged.count = candidate.recordCount();
    expected.merged.recordSize = candidate.recordSize();
  }
  for (const auto* path :
       {MERGE_TINTA_JOURNAL_PATHS.events, MERGE_TINTA_JOURNAL_PATHS.headerA, MERGE_TINTA_JOURNAL_PATHS.headerB})
    state.files.erase(path);
  state.directories.erase(MERGE_TINTA_JOURNAL_PATHS.directory);
  {
    HalJournalMergeCandidateSession session;
    ASSERT_EQ(session.begin(expected, expected.generation), TintaJournalResult::Ok);
    ASSERT_EQ(session.append(event, body), TintaJournalResult::Ok);
    ASSERT_EQ(session.commit(expected.generation, nullptr, nullptr), TintaJournalResult::Ok);
  }
  const auto committed = state.files.at(TINTA_JOURNAL_EVENTS);
  {
    HalJournalMergeCandidateSession reconnected;
    EXPECT_EQ(reconnected.begin(expected, expected.generation), TintaJournalResult::Duplicate);
    EXPECT_EQ(state.files.at(TINTA_JOURNAL_EVENTS), committed);
  }
  HalJournalCausalAuditSession audit;
  PortablePreferenceResolution resolution;
  ASSERT_TRUE(audit.run());
  EXPECT_EQ(audit.recordCount(), 1u);
  ASSERT_EQ(audit.resolvePortablePreferences(resolution), TintaJournalResult::Ok);
  ASSERT_EQ(resolution.bodies().size(), 1u);
  EXPECT_EQ(resolution.bodies()[0][4], 20);
}

TEST(HalTintaJournalStorageTest, CandidateCommitRefusesUnresolvedPortableMarginBeforePublication) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
  std::array<uint8_t, 8> first{1, 4, 8, 1, 10, 0, 0, 0};
  auto second = first;
  second[4] = 20;
  SyncEvent event;
  event.identity.epoch = event.identity.sequence = 1;
  event.storageGeneration.fill(2);
  event.kind = EventKind::Preference;
  event.resource = PREFERENCE_SCOPE;
  for (const auto location : {TintaJournalLocation::Active, TintaJournalLocation::MergeCandidate}) {
    HalTintaJournalStorage storage(location);
    std::array<uint8_t, 1024> scratch{};
    TintaJournal journal(storage, scratch);
    ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
    event.identity.origin.fill(1);
    ASSERT_TRUE(storage.digest(first, event.bodyHash));
    ASSERT_EQ(journal.append(event, first), TintaJournalResult::Ok);
    if (location == TintaJournalLocation::MergeCandidate) {
      event.identity.origin.fill(3);
      ASSERT_TRUE(storage.digest(second, event.bodyHash));
      ASSERT_EQ(journal.append(event, second), TintaJournalResult::Ok);
    }
  }
  auto expected = mergeIntent();
  {
    HalJournalCausalAuditSession active, candidate(TintaJournalLocation::MergeCandidate);
    ASSERT_TRUE(active.run(&expected.previous.frontier));
    expected.previous.count = active.recordCount();
    expected.previous.recordSize = active.recordSize();
    ASSERT_TRUE(candidate.run(&expected.merged.frontier));
    expected.merged.count = candidate.recordCount();
    expected.merged.recordSize = candidate.recordSize();
  }
  const auto original = state.files.at(TINTA_JOURNAL_EVENTS);
  for (const auto* path :
       {MERGE_TINTA_JOURNAL_PATHS.events, MERGE_TINTA_JOURNAL_PATHS.headerA, MERGE_TINTA_JOURNAL_PATHS.headerB})
    state.files.erase(path);
  state.directories.erase(MERGE_TINTA_JOURNAL_PATHS.directory);
  HalJournalMergeCandidateSession session;
  ASSERT_EQ(session.begin(expected, expected.generation), TintaJournalResult::Ok);
  ASSERT_TRUE(session.available());
  ASSERT_EQ(session.append(event, second), TintaJournalResult::Ok);
  EXPECT_EQ(session.commit(expected.generation, nullptr, nullptr), TintaJournalResult::Conflict);
  EXPECT_FALSE(session.available());
  EXPECT_EQ(state.files.at(TINTA_JOURNAL_EVENTS), original);
  EXPECT_TRUE(state.files.contains(MERGE_TINTA_JOURNAL_PATHS.events));
  ASSERT_EQ(session.abort(expected, expected.generation), TintaJournalResult::Ok);
  EXPECT_FALSE(session.available());
  EXPECT_EQ(state.files.at(TINTA_JOURNAL_EVENTS), original);

  HalJournalMergeReceiveSession receiver;
  expected.transaction[0] ^= 1;
  std::array<uint8_t, JOURNAL_MERGE_INTENT_SIZE> declaration{};
  ASSERT_TRUE(encodeJournalMergeIntent(expected, declaration));
  std::array<uint8_t, MAX_CONTROL_PAYLOAD> payload{};
  JournalMergeRequestView request;
  request.transaction = expected.transaction;
  request.declaration = declaration;
  auto dispatch = [&]() {
    const auto size = encodeJournalMergeRequest(request, payload);
    EXPECT_NE(size, 0u);
    return receiver.dispatch(std::span(payload).first(size), expected.owner, expected.generation, nullptr, nullptr);
  };
  ASSERT_EQ(dispatch(), TintaJournalResult::Ok);
  ASSERT_NE(receiver.receivingDeclaration(), nullptr);
  std::array<uint8_t, MAX_RECORD_SIZE> envelope{};
  const auto envelopeSize = encodeRecord(event, envelope);
  ASSERT_NE(envelopeSize, 0u);
  request.operation = JournalMergeOperation::Append;
  request.declaration = {};
  request.envelope = std::span(envelope).first(envelopeSize);
  request.body = second;
  ASSERT_EQ(dispatch(), TintaJournalResult::Ok);
  request.operation = JournalMergeOperation::Commit;
  request.declaration = declaration;
  request.envelope = request.body = {};
  EXPECT_EQ(dispatch(), TintaJournalResult::Conflict);
  EXPECT_TRUE(receiver.hasBinding());
  EXPECT_EQ(receiver.receivingDeclaration(), nullptr);
  EXPECT_EQ(state.files.at(TINTA_JOURNAL_EVENTS), original);
  request.operation = JournalMergeOperation::Begin;
  ASSERT_EQ(dispatch(), TintaJournalResult::Ok);
  EXPECT_NE(receiver.receivingDeclaration(), nullptr);
  request.operation = JournalMergeOperation::Abort;
  ASSERT_EQ(dispatch(), TintaJournalResult::Ok);
  EXPECT_FALSE(receiver.hasBinding());
  EXPECT_EQ(receiver.receivingDeclaration(), nullptr);
}

TEST(HalTintaJournalStorageTest, KnowledgeHeadsStreamEveryBranchAndRequireSuccessfulCheckedCloses) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  HalTintaJournalStorage storage;
  std::array<uint8_t, 1024> scratch{};
  TintaJournal journal(storage, scratch);
  ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
  std::array<uint8_t, 8> body{1, 4, 8, 1, 20, 0, 0, 0};
  SyncEvent event;
  event.identity.epoch = 1;
  event.identity.sequence = 1;
  event.storageGeneration.fill(2);
  event.kind = EventKind::Preference;
  event.resource = PREFERENCE_SCOPE;
  ASSERT_TRUE(storage.digest(body, event.bodyHash));
  for (uint8_t origin = 1; origin <= 6; ++origin) {
    event.identity.origin.fill(origin);
    ASSERT_EQ(journal.append(event, body), TintaJournalResult::Ok);
  }
  struct Capture {
    uint32_t count = 0;
    bool reject = false;
    bool failClose = false;
    static bool visit(void* context, const EventIdentity& id) {
      auto& self = *static_cast<Capture*>(context);
      ++self.count;
      EXPECT_EQ(id.origin[0], self.count);
      if (self.failClose) inventory_hal_test::state.failClose = true;
      return !self.reject;
    }
  } capture;
  HalJournalCausalAuditSession audit;
  EXPECT_FALSE(audit.knowledgeHeads(&capture, Capture::visit));
  ASSERT_TRUE(audit.run());
  ASSERT_TRUE(audit.knowledgeHeads(&capture, Capture::visit));
  EXPECT_EQ(capture.count, 6U);
  EXPECT_FALSE(audit.knowledgeHeads(&capture, Capture::visit));
  capture = {};
  capture.reject = true;
  ASSERT_TRUE(audit.run());
  EXPECT_FALSE(audit.knowledgeHeads(&capture, Capture::visit));
  EXPECT_EQ(capture.count, 1U);
  capture = {};
  ASSERT_TRUE(audit.run());
  state.readErrorPath = TINTA_JOURNAL_EVENTS;
  EXPECT_FALSE(audit.knowledgeHeads(&capture, Capture::visit));
  EXPECT_EQ(capture.count, 0U);
  state.readErrorPath.clear();
  ASSERT_TRUE(audit.run());
  capture.failClose = true;
  EXPECT_FALSE(audit.knowledgeHeads(&capture, Capture::visit));
  EXPECT_GT(capture.count, 0U);
  state.failClose = false;
}

namespace {
class KnowledgeSnapshotIdentities final : public IdentityStorage {
 public:
  uint8_t random = 8;
  bool hardwareIdentity(Identity& value) override {
    value.fill(9);
    return true;
  }
  bool cardIdentity(Identity& value) override {
    value.fill(2);
    return true;
  }
  IdentityRead readBinding(std::span<uint8_t>) override { return IdentityRead::Missing; }
  bool writeBinding(std::span<const uint8_t>) override { return true; }
  IdentityRead readMarker(Identity&) override { return IdentityRead::Missing; }
  bool createMarker(const Identity&) override { return true; }
  bool randomIdentity(Identity& value) override {
    value.fill(random++);
    return true;
  }
};
}  // namespace
TEST(HalTintaJournalStorageTest, SdKnowledgeSnapshotRemainsStableWhilePreferenceWriterAppendsBatches) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  HalTintaJournalStorage storage;
  std::array<uint8_t, 1024> scratch{};
  TintaJournal journal(storage, scratch);
  ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
  std::array<uint8_t, 8> body{1, 4, 8, 1, 20, 0, 0, 0};
  SyncEvent event;
  event.identity.epoch = event.identity.sequence = 1;
  event.storageGeneration.fill(2);
  event.kind = EventKind::Preference;
  event.resource = PREFERENCE_SCOPE;
  for (uint8_t origin = 6; origin; --origin) {
    event.identity.origin.fill(origin);
    body[4] = origin * 5;
    ASSERT_TRUE(storage.digest(body, event.bodyHash));
    ASSERT_EQ(journal.append(event, body), TintaJournalResult::Ok);
  }
  HalJournalCausalAuditSession audit;
  EXPECT_EQ(audit.beginPreferenceKnowledge(6), nullptr);
  ASSERT_TRUE(audit.run());
  KnowledgeSnapshotIdentities identities;
  TintaWriter writer(journal, storage);
  ASSERT_EQ(writer.start(identities), TintaJournalResult::Ok);
  EXPECT_EQ(audit.beginPreferenceKnowledge(7), nullptr);
  auto* snapshot = audit.beginPreferenceKnowledge(journal.count());
  ASSERT_NE(snapshot, nullptr);
  EXPECT_EQ(snapshot->count(), 6U);
  const auto indexBefore = state.files.at(HalJournalIdentityIndexStorage::PATH);
  EXPECT_FALSE(audit.run());
  EXPECT_EQ(state.files.at(HalJournalIdentityIndexStorage::PATH), indexBefore);
  EXPECT_FALSE(audit.endExport());
  Digest frontier;
  EXPECT_FALSE(audit.prefixFrontier(6, frontier));
  body[4] = 40;
  ASSERT_EQ(writer.recordPreferenceResolving(body, *snapshot, 0, 0, ClockQuality::Unknown), TintaJournalResult::Ok);
  EXPECT_EQ(snapshot->count(), 6U);
  EXPECT_EQ(journal.count(), 8U);
  EXPECT_EQ(state.files.at(HalJournalIdentityIndexStorage::PATH), indexBefore);
  ASSERT_TRUE(audit.endPreferenceKnowledge());
  ASSERT_TRUE(audit.endPreferenceKnowledge());
  ASSERT_TRUE(audit.run());
  PortablePreferenceResolution resolution;
  ASSERT_EQ(audit.resolvePortablePreferences(resolution), TintaJournalResult::Ok);
  ASSERT_EQ(resolution.bodies().size(), 1U);
  EXPECT_EQ(resolution.bodies()[0][4], 40);
}
TEST(HalTintaJournalStorageTest, SdKnowledgeSnapshotRefusesReadAndCloseFailuresAndSupportsEmptyJournal) {
  auto& state = inventory_hal_test::state;
  for (const bool failIndex : {false, true}) {
    state = {};
    state.enumerateFileMap = true;
    HalTintaJournalStorage storage;
    std::array<uint8_t, 1024> scratch{};
    TintaJournal journal(storage, scratch);
    ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
    std::array<uint8_t, 8> body{1, 4, 8, 1, 20, 0, 0, 0};
    SyncEvent event;
    event.identity.origin.fill(1);
    event.identity.epoch = event.identity.sequence = 1;
    event.storageGeneration.fill(2);
    event.kind = EventKind::Preference;
    event.resource = PREFERENCE_SCOPE;
    ASSERT_TRUE(storage.digest(body, event.bodyHash));
    ASSERT_EQ(journal.append(event, body), TintaJournalResult::Ok);
    HalJournalCausalAuditSession audit;
    ASSERT_TRUE(audit.run());
    auto* snapshot = audit.beginPreferenceKnowledge(1);
    ASSERT_NE(snapshot, nullptr);
    EventIdentity output;
    output.origin.fill(99);
    const auto original = output;
    state.readErrorPath = failIndex ? HalJournalIdentityIndexStorage::PATH : HalJournalReplayVisits::PATH;
    EXPECT_FALSE(snapshot->read(0, output));
    EXPECT_EQ(output, original);
    EXPECT_EQ(snapshot->count(), 0U);
    state.readErrorPath.clear();
    EXPECT_TRUE(audit.endPreferenceKnowledge());
    ASSERT_TRUE(audit.run());
    snapshot = audit.beginPreferenceKnowledge(1);
    ASSERT_NE(snapshot, nullptr);
    state.failClose = true;
    EXPECT_FALSE(audit.endPreferenceKnowledge());
    state.failClose = false;
  }
  state = {};
  state.enumerateFileMap = true;
  HalJournalCausalAuditSession empty;
  ASSERT_TRUE(empty.run());
  auto* snapshot = empty.beginPreferenceKnowledge(0);
  ASSERT_NE(snapshot, nullptr);
  EXPECT_EQ(snapshot->count(), 0U);
  ASSERT_TRUE(empty.endPreferenceKnowledge());
}

TEST(HalTintaJournalStorageTest, BookmarkResolutionRequiresAuditAndCheckedClosesBeforePublication) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  HalTintaJournalStorage storage;
  std::array<uint8_t, 1024> scratch{};
  TintaJournal journal(storage, scratch);
  ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
  BookmarkBodyView bookmark;
  bookmark.identity[0] = 7;
  bookmark.anchor = {2, 123};
  std::array<uint8_t, MAX_BOOKMARK_BODY_SIZE> body{};
  const auto size = encodeBookmarkBody(bookmark, body);
  SyncEvent event;
  event.identity.origin.fill(1);
  event.identity.epoch = event.identity.sequence = 1;
  event.storageGeneration.fill(2);
  event.kind = EventKind::BookmarkPut;
  event.resource.fill(8);
  ASSERT_TRUE(storage.digest(std::span(body).first(size), event.bodyHash));
  ASSERT_EQ(journal.append(event, std::span(body).first(size)), TintaJournalResult::Ok);
  HalJournalCausalAuditSession audit;
  std::array<uint8_t, MAX_BOOKMARK_BODY_SIZE> output;
  output.fill(0xa5);
  const auto untouched = output;
  size_t length = 99;
  EXPECT_EQ(audit.resolveBookmark(event.resource, bookmark.identity, output, length), TintaJournalResult::Unavailable);
  EXPECT_EQ(output, untouched);
  EXPECT_EQ(length, 99u);
  ASSERT_TRUE(audit.run());
  state.failClose = true;
  EXPECT_EQ(audit.resolveBookmark(event.resource, bookmark.identity, output, length), TintaJournalResult::IoError);
  EXPECT_EQ(output, untouched);
  EXPECT_EQ(length, 99u);
  state.failClose = false;
  ASSERT_TRUE(audit.run());
  ASSERT_EQ(audit.resolveBookmark(event.resource, bookmark.identity, output, length), TintaJournalResult::Ok);
  EXPECT_EQ(length, size);
  EXPECT_TRUE(std::equal(output.begin(), output.begin() + length, body.begin()));
}

TEST(HalTintaJournalStorageTest, BookmarkEnumerationRequiresAuditYieldsAndIncludesDeletionIds) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  HalTintaJournalStorage storage;
  std::array<uint8_t, 1024> scratch{};
  TintaJournal journal(storage, scratch);
  ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
  SyncEvent event;
  event.identity.origin.fill(1);
  event.identity.epoch = 1;
  event.storageGeneration.fill(2);
  event.resource.fill(8);
  BookmarkBodyView bookmark;
  std::array<uint8_t, MAX_BOOKMARK_BODY_SIZE> body{};
  for (unsigned at = 0; at < 70; ++at) {
    bookmark.identity[0] = at % 2 ? 7 : 3;
    bookmark.deleted = bookmark.identity[0] == 7;
    event.kind = bookmark.deleted ? EventKind::BookmarkDelete : EventKind::BookmarkPut;
    event.identity.sequence = at + 1;
    const auto length = encodeBookmarkBody(bookmark, body);
    ASSERT_TRUE(storage.digest(std::span(body).first(length), event.bodyHash));
    ASSERT_EQ(journal.append(event, std::span(body).first(length)), TintaJournalResult::Ok);
  }
  ASSERT_TRUE(storage.close());
  std::vector<Identity> staged;
  staged.reserve(2);
  const auto visitor = [](void* context, const Identity& identity) {
    static_cast<std::vector<Identity>*>(context)->push_back(identity);
    return true;
  };
  HalJournalCausalAuditSession audit;
  EXPECT_EQ(audit.bookmarkIdentities(event.resource, &staged, visitor), TintaJournalResult::Unavailable);
  EXPECT_TRUE(staged.empty());
  ASSERT_TRUE(audit.run());
  const auto yields = state.yields;
  ASSERT_EQ(audit.bookmarkIdentities(event.resource, &staged, visitor), TintaJournalResult::Ok);
  ASSERT_EQ(staged.size(), 2u);
  EXPECT_EQ(staged[0][0], 3);
  EXPECT_EQ(staged[1][0], 7);
  EXPECT_GE(state.yields - yields, 5u);
  EXPECT_EQ(audit.bookmarkIdentities(event.resource, &staged, visitor), TintaJournalResult::Unavailable);
  EXPECT_EQ(staged.size(), 2u);
}

TEST(HalTintaJournalStorageTest, BookmarkEnumerationRefusesVisitorReadAndCloseFailures) {
  for (unsigned mode = 0; mode < 3; ++mode) {
    auto& state = inventory_hal_test::state;
    state = {};
    state.enumerateFileMap = true;
    HalTintaJournalStorage storage;
    std::array<uint8_t, 1024> scratch{};
    TintaJournal journal(storage, scratch);
    ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
    SyncEvent event;
    event.identity.origin.fill(1);
    event.identity.epoch = event.identity.sequence = 1;
    event.storageGeneration.fill(2);
    event.kind = EventKind::BookmarkDelete;
    event.resource.fill(8);
    BookmarkBodyView bookmark;
    bookmark.identity[0] = 3;
    bookmark.deleted = true;
    std::array<uint8_t, 18> body{};
    ASSERT_EQ(encodeBookmarkBody(bookmark, body), body.size());
    ASSERT_TRUE(storage.digest(body, event.bodyHash));
    ASSERT_EQ(journal.append(event, body), TintaJournalResult::Ok);
    ASSERT_TRUE(storage.close());
    HalJournalCausalAuditSession audit;
    ASSERT_TRUE(audit.run());
    if (mode == 1) state.readErrorPath = TINTA_JOURNAL_EVENTS;
    if (mode == 2) state.failClose = true;
    const auto visitor = [](void* context, const Identity&) { return *static_cast<unsigned*>(context) != 0; };
    EXPECT_EQ(audit.bookmarkIdentities(event.resource, &mode, visitor), TintaJournalResult::IoError);
    state.failClose = false;
    state.readErrorPath.clear();
  }
}

TEST(HalTintaJournalStorageTest, BookmarkIdStageVerifiesSortedSpoolAndCheckedCleanup) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  std::array<uint8_t, 128> scratch{};
  auto stage = makeUniqueNoThrow<HalBookmarkIdentityStage>(scratch);
  ASSERT_TRUE(stage);
  ASSERT_TRUE(stage->begin(2));
  Identity first{}, second{}, output{};
  first[0] = 3;
  second[0] = 7;
  ASSERT_TRUE(stage->append(first));
  ASSERT_TRUE(stage->append(second));
  ASSERT_TRUE(stage->seal());
  EXPECT_EQ(stage->size(), 2u);
  EXPECT_EQ(stage->next(output), BookmarkCursorResult::Found);
  EXPECT_EQ(output, first);
  EXPECT_EQ(stage->next(output), BookmarkCursorResult::Found);
  EXPECT_EQ(output, second);
  EXPECT_EQ(stage->next(output), BookmarkCursorResult::End);
  EXPECT_EQ(stage->next(output), BookmarkCursorResult::End);
  EXPECT_EQ(output, second);
  EXPECT_TRUE(state.files.count(HalBookmarkIdentityStage::PATH));
  ASSERT_TRUE(stage->cleanup());
  EXPECT_FALSE(state.files.count(HalBookmarkIdentityStage::PATH));
  ASSERT_TRUE(stage->begin(0));
  ASSERT_TRUE(stage->seal());
  EXPECT_EQ(stage->next(output), BookmarkCursorResult::End);
  EXPECT_EQ(output, second);
}

TEST(HalTintaJournalStorageTest, BookmarkIdStagePreservesForeignFilesAndRejectsCorruptReadback) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  std::array<uint8_t, 128> scratch{};
  state.files[HalBookmarkIdentityStage::PATH] = {1, 2, 3};
  const auto original = state.files;
  auto stage = makeUniqueNoThrow<HalBookmarkIdentityStage>(scratch);
  ASSERT_TRUE(stage);
  EXPECT_FALSE(stage->begin(2));
  stage.reset();
  EXPECT_EQ(state.files, original);
  state = {};
  state.enumerateFileMap = true;
  stage = makeUniqueNoThrow<HalBookmarkIdentityStage>(scratch);
  ASSERT_TRUE(stage);
  ASSERT_TRUE(stage->begin(1));
  Identity identity{};
  identity[0] = 3;
  ASSERT_TRUE(stage->append(identity));
  ASSERT_TRUE(stage->seal());
  state.files.at(HalBookmarkIdentityStage::PATH)[1] ^= 1;
  Identity output{};
  EXPECT_EQ(stage->next(output), BookmarkCursorResult::Found);
  EXPECT_EQ(stage->next(output), BookmarkCursorResult::Error);
  EXPECT_TRUE(stage->cleanup());
}

TEST(HalTintaJournalStorageTest, BookmarkIdStageRefusesDuplicatesBoundsAndFailedFinalClose) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  std::array<uint8_t, 128> scratch{};
  auto stage = makeUniqueNoThrow<HalBookmarkIdentityStage>(scratch);
  ASSERT_TRUE(stage);
  Identity identity{};
  identity[0] = 3;
  ASSERT_TRUE(stage->begin(2));
  ASSERT_TRUE(stage->append(identity));
  EXPECT_FALSE(stage->append(identity));
  EXPECT_FALSE(stage->seal());
  ASSERT_TRUE(stage->cleanup());
  ASSERT_TRUE(stage->begin(1));
  ASSERT_TRUE(stage->append(identity));
  identity[0] = 7;
  EXPECT_FALSE(stage->append(identity));
  ASSERT_TRUE(stage->cleanup());
  ASSERT_TRUE(stage->begin(1));
  ASSERT_TRUE(stage->append(identity));
  ASSERT_TRUE(stage->seal());
  Identity output{};
  EXPECT_EQ(stage->next(output), BookmarkCursorResult::Found);
  EXPECT_EQ(output, identity);
  state.failClose = true;
  EXPECT_EQ(stage->next(output), BookmarkCursorResult::Error);
  EXPECT_EQ(output, identity);
  state.failClose = false;
  state.failRemove = true;
  EXPECT_FALSE(stage->cleanup());
  EXPECT_TRUE(state.files.count(HalBookmarkIdentityStage::PATH));
  state.failRemove = false;
  EXPECT_TRUE(stage->cleanup());
  EXPECT_FALSE(state.files.count(HalBookmarkIdentityStage::PATH));
}

TEST(HalTintaJournalStorageTest, SdKnowledgeSnapshotFiltersParentsAndRejectsOutOfOrderReads) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  HalTintaJournalStorage storage;
  std::array<uint8_t, 1024> scratch{};
  TintaJournal journal(storage, scratch);
  ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
  std::array<uint8_t, 8> body{1, 4, 8, 1, 20, 0, 0, 0};
  SyncEvent event;
  event.identity.origin.fill(1);
  event.identity.epoch = event.identity.sequence = 1;
  event.storageGeneration.fill(2);
  event.kind = EventKind::Preference;
  event.resource = PREFERENCE_SCOPE;
  ASSERT_TRUE(storage.digest(body, event.bodyHash));
  ASSERT_EQ(journal.append(event, body), TintaJournalResult::Ok);
  const auto parent = event.identity;
  event.identity.sequence = 2;
  ASSERT_EQ(journal.append(event, body), TintaJournalResult::Ok);
  const auto firstHead = event.identity;
  event.identity.origin.fill(2);
  event.identity.sequence = 1;
  event.ancestorCount = 1;
  event.ancestors[0] = parent;
  ASSERT_EQ(journal.append(event, body), TintaJournalResult::Ok);
  const auto secondHead = event.identity;
  HalJournalCausalAuditSession audit;
  ASSERT_TRUE(audit.run());
  auto* snapshot = audit.beginPreferenceKnowledge(3);
  ASSERT_NE(snapshot, nullptr);
  EXPECT_EQ(snapshot->count(), 2U);
  EventIdentity output;
  output.origin.fill(99);
  const auto original = output;
  EXPECT_FALSE(snapshot->read(1, output));
  EXPECT_EQ(output, original);
  EXPECT_EQ(snapshot->count(), 0U);
  ASSERT_TRUE(audit.endPreferenceKnowledge());
  ASSERT_TRUE(audit.run());
  snapshot = audit.beginPreferenceKnowledge(3);
  ASSERT_NE(snapshot, nullptr);
  ASSERT_TRUE(snapshot->read(0, output));
  EXPECT_EQ(output, firstHead);
  ASSERT_TRUE(snapshot->read(1, output));
  EXPECT_EQ(output, secondHead);
  ASSERT_TRUE(audit.endPreferenceKnowledge());
}
