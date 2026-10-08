#include <gtest/gtest.h>
#include <openssl/evp.h>

#include <fstream>
#include <iterator>
#include <optional>

#include "../../lib/Companion/CompanionTintaReceiveCheckpoint.h"
#include "../../lib/hal/HalDictionaryBundleHashStage.h"
#include "../../lib/hal/HalInventoryFileHash.h"
#include "../../lib/hal/HalReaderPreferenceFontFileProof.h"
#include "../../lib/hal/HalTintaDerivedCandidateStage.h"
#include "../../lib/hal/HalTintaDerivedFileOwnership.h"
#include "../../lib/hal/HalTintaDerivedFileVerification.h"
#include "../../lib/hal/HalTintaDerivedGenerationValidation.h"
#include "../../lib/hal/HalTintaDerivedIntentWriter.h"
#include "../../lib/hal/HalTintaDerivedPublicationStorage.h"
#include "../../lib/hal/HalTintaDerivedReceiptWriter.h"
#include "../../lib/hal/HalTintaDerivedRecordReader.h"
#include "../../lib/hal/HalTintaDerivedRecovery.h"
#include "../../lib/hal/HalTintaItemSnapshotValidation.h"
#include "../../lib/hal/HalTintaReceiveJournalStorage.h"
#include "lib/Tinta/src/core/srs/Bytes.h"
using namespace companion;
class InventoryHashTest : public testing::Test {
 protected:
  void SetUp() override { inventory_hal_test::state = {}; }
};
TEST_F(InventoryHashTest, HalReceiveJournalSyncFailureBlocksWritesAndRestartRecoversCompleteCheckpoint) {
  TintaReceiveCheckpoint initial;
  initial.course.fill(7);
  initial.storage.fill(1);
  initial.transaction.fill(8);
  initial.owner.fill(9);
  initial.manifestHash.fill(5);
  initial.length = 1056;
  initial.sequence = 1;
  std::array<char, COURSE_STATE_DIRECTORY_SIZE> root{};
  ASSERT_TRUE(courseStateDirectory(initial.course, root));
  auto& state = inventory_hal_test::state;
  state.directories[root.data()] = {};
  state.enumerateFileMap = true;
  std::array<uint8_t, TINTA_RECEIVE_CHECKPOINT_SIZE> scratch{};
  HalTintaReceiveJournalStorage storage(initial.course);
  TintaReceiveJournal journal(storage, scratch);
  ASSERT_EQ(journal.open(), TintaReceiveJournalResult::Missing);
  ASSERT_EQ(journal.start(initial), TintaReceiveJournalResult::Ok);
  auto next = initial;
  next.sequence = 2;
  next.offset = 100;
  state.failSync = true;
  EXPECT_EQ(journal.advance(next), TintaReceiveJournalResult::IoError);
  EXPECT_EQ(journal.checkpoint(), nullptr);
  EXPECT_EQ(journal.advance(next), TintaReceiveJournalResult::Unavailable);
  state.failSync = false;
  HalTintaReceiveJournalStorage restarted(initial.course);
  TintaReceiveJournal recovered(restarted, scratch);
  ASSERT_EQ(recovered.open(), TintaReceiveJournalResult::Ok);
  EXPECT_EQ(*recovered.checkpoint(), next);
  ASSERT_TRUE(encodeTintaReceiveCheckpoint(next, scratch));
  const auto before = state.files;
  EXPECT_FALSE(restarted.write(1, scratch));
  EXPECT_EQ(state.files, before);
  state.failRead = state.reads + 1;
  EXPECT_EQ(recovered.open(), TintaReceiveJournalResult::IoError);
  EXPECT_EQ(recovered.checkpoint(), nullptr);
}
TEST_F(InventoryHashTest, ForeignCourseReceiveSlotBlocksRecoveryWithoutFallbackOrWrites) {
  TintaReceiveCheckpoint initial;
  initial.course.fill(7);
  initial.storage.fill(1);
  initial.transaction.fill(8);
  initial.owner.fill(9);
  initial.manifestHash.fill(5);
  initial.length = 1056;
  initial.sequence = 1;
  std::array<char, COURSE_STATE_DIRECTORY_SIZE> root{};
  std::array<char, COURSE_STATE_PATH_SIZE> path{};
  ASSERT_TRUE(courseStateDirectory(initial.course, root));
  ASSERT_TRUE(courseStatePath(initial.course, "receive-a", path));
  auto& state = inventory_hal_test::state;
  state.directories[root.data()] = {};
  state.enumerateFileMap = true;
  std::array<uint8_t, TINTA_RECEIVE_CHECKPOINT_SIZE> scratch{};
  HalTintaReceiveJournalStorage storage(initial.course);
  TintaReceiveJournal journal(storage, scratch);
  ASSERT_EQ(journal.open(), TintaReceiveJournalResult::Missing);
  ASSERT_EQ(journal.start(initial), TintaReceiveJournalResult::Ok);
  auto foreign = initial;
  foreign.course.fill(6);
  foreign.sequence = 2;
  foreign.offset = 37;
  ASSERT_TRUE(encodeTintaReceiveCheckpoint(foreign, scratch));
  state.files[path.data()] = std::vector<uint8_t>(scratch.begin(), scratch.end());
  const auto before = state.files;
  EXPECT_EQ(journal.open(), TintaReceiveJournalResult::IoError);
  EXPECT_EQ(journal.checkpoint(), nullptr);
  EXPECT_EQ(journal.advance(initial), TintaReceiveJournalResult::Unavailable);
  EXPECT_EQ(state.files, before);
}
TEST_F(InventoryHashTest, ExistingEmptyReceiveSlotIsCorruptionNotMissing) {
  Identity course{};
  course.fill(7);
  std::array<char, COURSE_STATE_DIRECTORY_SIZE> root{};
  std::array<char, COURSE_STATE_PATH_SIZE> path{};
  ASSERT_TRUE(courseStateDirectory(course, root));
  ASSERT_TRUE(courseStatePath(course, "receive-b", path));
  auto& state = inventory_hal_test::state;
  state.directories[root.data()] = {};
  state.enumerateFileMap = true;
  state.files[path.data()] = {};
  std::array<uint8_t, TINTA_RECEIVE_CHECKPOINT_SIZE> scratch{};
  HalTintaReceiveJournalStorage storage(course);
  TintaReceiveJournal journal(storage, scratch);
  const auto before = state.files;
  EXPECT_EQ(journal.open(), TintaReceiveJournalResult::Corrupt);
  EXPECT_EQ(state.files, before);
}
TEST_F(InventoryHashTest, ReceiveCheckpointMustMatchExactManifestReceiptAndAuthenticatedBindings) {
  std::ifstream input(TINTA_DERIVED_FIXTURE, std::ios::binary);
  ASSERT_TRUE(input.good());
  const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
  TintaDerivedManifestView manifest;
  ASSERT_TRUE(manifest.decode(bytes));
  TintaReceiveCheckpoint checkpoint;
  checkpoint.course.fill(7);
  checkpoint.storage.fill(1);
  checkpoint.transaction.fill(8);
  checkpoint.owner.fill(9);
  checkpoint.file = TintaDerivedFile::Days;
  checkpoint.sealedMask = 15;
  checkpoint.length = 4;
  checkpoint.offset = 3;
  checkpoint.sequence = 1;
  Digest hash{}, pack{}, frontier{};
  pack.fill(3);
  frontier.fill(4);
  ASSERT_NE(SHA256(bytes.data(), bytes.size(), hash.data()), nullptr);
  checkpoint.manifestHash = hash;
  const auto original = checkpoint;
  auto matches = [&] {
    return matchesTintaReceiveCheckpoint(checkpoint, manifest, hash, original.transaction, original.owner,
                                         original.course, original.storage, pack, frontier);
  };
  ASSERT_TRUE(matches());
  for (unsigned field = 0; field < 8; ++field) {
    checkpoint = original;
    switch (field) {
      case 0:
        checkpoint.course[0] ^= 1;
        break;
      case 1:
        checkpoint.storage[0] ^= 1;
        break;
      case 2:
        checkpoint.transaction[0] ^= 1;
        break;
      case 3:
        checkpoint.owner[0] ^= 1;
        break;
      case 4:
        checkpoint.manifestHash[0] ^= 1;
        break;
      case 5:
        checkpoint.length = 16;
        break;
      case 6:
        checkpoint.file = TintaDerivedFile::Items;
        break;
      case 7:
        checkpoint.offset = 5;
        break;
    }
    EXPECT_FALSE(matches()) << field;
  }
  checkpoint = original;
  pack[0] ^= 1;
  EXPECT_FALSE(matches());
  pack.fill(3);
  frontier[0] ^= 1;
  EXPECT_FALSE(matches());
  frontier.fill(4);
  TintaDerivedManifestView unavailable;
  EXPECT_FALSE(matchesTintaReceiveCheckpoint(checkpoint, unavailable, hash, original.transaction, original.owner,
                                             original.course, original.storage, pack, frontier));
}
TEST_F(InventoryHashTest, LoadsRecoveryManifestWithoutPriorBytesAndPreservesOutputOnFailure) {
  std::ifstream input(TINTA_DERIVED_FIXTURE, std::ios::binary);
  ASSERT_TRUE(input.good());
  const std::vector<uint8_t> expected{std::istreambuf_iterator<char>(input), {}};
  Identity course{};
  course.fill(7);
  std::array<char, COURSE_STATE_DIRECTORY_SIZE> root{};
  std::array<char, COURSE_STATE_PATH_SIZE> path{};
  ASSERT_TRUE(courseStateDirectory(course, root));
  ASSERT_TRUE(tintaDerivedRecordPath(course, TintaDerivedRecord::Intent, path));
  auto& state = inventory_hal_test::state;
  state.directories[root.data()] = {};
  state.enumerateFileMap = true;
  std::array<uint8_t, 512> scratch{};
  std::array<uint8_t, TINTA_DERIVED_MANIFEST_SIZE> output{};
  output.fill(42);
  const auto original = output;
  HalTintaDerivedRecordReader reader(course, scratch);
  EXPECT_EQ(reader.load(TintaDerivedRecord::Intent, output), TintaDerivedRecordLoad::Missing);
  EXPECT_EQ(output, original);
  state.files[path.data()] = expected;
  ASSERT_EQ(reader.load(TintaDerivedRecord::Intent, output), TintaDerivedRecordLoad::Loaded);
  EXPECT_TRUE(std::equal(output.begin(), output.end(), expected.begin()));
  output = original;
  state.files[path.data()].back() ^= 1;
  EXPECT_EQ(reader.load(TintaDerivedRecord::Intent, output), TintaDerivedRecordLoad::Invalid);
  EXPECT_EQ(output, original);
  state.files[path.data()] = expected;
  state.failRead = state.reads + 1;
  EXPECT_EQ(reader.load(TintaDerivedRecord::Intent, output), TintaDerivedRecordLoad::IoError);
  EXPECT_EQ(output, original);
  state.failRead = 0;
  EXPECT_EQ(reader.load(TintaDerivedRecord::Intent, scratch), TintaDerivedRecordLoad::IoError);
}
TEST_F(InventoryHashTest, RecoveryRejectsInvalidBindingsBuffersAndRecordsWithoutMutatingFiles) {
  std::ifstream input(TINTA_DERIVED_FIXTURE, std::ios::binary);
  ASSERT_TRUE(input.good());
  const std::vector<uint8_t> expected{std::istreambuf_iterator<char>(input), {}};
  TintaDerivedPublicationBindings bindings;
  bindings.course.fill(7);
  bindings.storage.fill(1);
  bindings.pack.fill(3);
  bindings.frontier.fill(4);
  std::array<char, COURSE_STATE_DIRECTORY_SIZE> root{};
  std::array<char, COURSE_STATE_PATH_SIZE> intent{}, receipt{};
  ASSERT_TRUE(courseStateDirectory(bindings.course, root));
  ASSERT_TRUE(tintaDerivedRecordPath(bindings.course, TintaDerivedRecord::Intent, intent));
  ASSERT_TRUE(tintaDerivedRecordPath(bindings.course, TintaDerivedRecord::Receipt, receipt));
  auto& state = inventory_hal_test::state;
  state.directories[root.data()] = {};
  state.enumerateFileMap = true;
  state.files[intent.data()] = expected;
  std::array<uint8_t, 512> scratch{};
  std::array<uint8_t, TINTA_DERIVED_MANIFEST_SIZE> pending{}, previous{};
  HalTintaDerivedRecordReader reader(bindings.course, scratch);
  const auto original = state.files;
  auto recover = [&] { return recoverTintaDerivedPublication(bindings, reader, pending, previous, scratch); };
  EXPECT_EQ(recover(), TintaDerivedRecoveryResult::Invalid);  // Missing journal proof.
  EXPECT_EQ(state.files, original);
  bindings.proveJournal = [](void*, const TintaDerivedManifestView&) { return false; };
  EXPECT_EQ(recover(), TintaDerivedRecoveryResult::Invalid);
  EXPECT_EQ(state.files, original);
  EXPECT_EQ(recoverTintaDerivedPublication(bindings, reader, pending, pending, scratch),
            TintaDerivedRecoveryResult::Invalid);
  EXPECT_EQ(recoverTintaDerivedPublication(bindings, reader, scratch, previous, scratch),
            TintaDerivedRecoveryResult::Invalid);
  EXPECT_EQ(recoverTintaDerivedPublication(bindings, reader, std::span(pending).first(331), previous, scratch),
            TintaDerivedRecoveryResult::Invalid);
  bindings.course.fill(8);
  EXPECT_EQ(recover(), TintaDerivedRecoveryResult::Invalid);
  bindings.course.fill(7);
  EXPECT_EQ(state.files, original);
  state.files[intent.data()].back() ^= 1;
  const auto corruptIntent = state.files;
  EXPECT_EQ(recover(), TintaDerivedRecoveryResult::Invalid);
  EXPECT_EQ(state.files, corruptIntent);
  state.files = original;
  state.failRead = state.reads + 1;
  EXPECT_EQ(recover(), TintaDerivedRecoveryResult::IoError);
  EXPECT_EQ(state.files, original);
  state.failRead = 0;
  state.files[receipt.data()] = expected;
  state.files[receipt.data()].back() ^= 1;
  const auto corruptReceipt = state.files;
  EXPECT_EQ(recover(), TintaDerivedRecoveryResult::Invalid);
  EXPECT_EQ(state.files, corruptReceipt);
  state.files = original;
  state.directoryErrorPath = root.data();
  EXPECT_EQ(recover(), TintaDerivedRecoveryResult::IoError);
  EXPECT_EQ(state.files, original);
}
TEST_F(InventoryHashTest, ComposedTintaReceptionPublishesAllFilesAndRequiresJournalProof) {
  std::ifstream input(TINTA_DERIVED_FIXTURE, std::ios::binary);
  ASSERT_TRUE(input.good());
  const std::vector<uint8_t> expected{std::istreambuf_iterator<char>(input), {}};
  TintaDerivedPublicationBindings bindings;
  bindings.course.fill(7);
  bindings.storage.fill(1);
  bindings.pack.fill(3);
  bindings.frontier.fill(4);
  std::array<char, COURSE_STATE_DIRECTORY_SIZE> root{};
  std::array<char, COURSE_STATE_PATH_SIZE> path{};
  ASSERT_TRUE(courseStateDirectory(bindings.course, root));
  auto& state = inventory_hal_test::state;
  state.directories[root.data()] = {};
  state.enumerateFileMap = true;
  std::string itemPath(TINTA_DERIVED_FIXTURE);
  itemPath.replace(itemPath.find("TintaDerivedManifest-v1.fixture"), std::string::npos, "TintaItemSnapshot-v1.fixture");
  std::ifstream itemFile(itemPath, std::ios::binary);
  ASSERT_TRUE(itemFile.good());
  std::array<std::vector<uint8_t>, 5> candidates;
  candidates[0] = {std::istreambuf_iterator<char>(itemFile), {}};
  candidates[4] = {'T', 'D', 'L', '1'};
  for (unsigned i = 2; i <= 3; ++i) {
    candidates[i].resize(16);
    std::memcpy(candidates[i].data(), "TCS1", 4);
    candidates[i][4] = i - 1;
    tinta::core::putU32(candidates[i].data() + 12, tinta::core::crc32(candidates[i].data(), 12));
  }
  std::array<uint8_t, 512> scratch{};
  TintaDerivedManifestView manifest;
  ASSERT_TRUE(manifest.decode(expected));
  for (unsigned i = 0; i < 5; ++i) {
    const auto kind = static_cast<TintaDerivedFile>(i);
    ASSERT_TRUE(tintaDerivedFilePath(bindings.course, kind, TintaDerivedRole::Active, path));
    state.files[path.data()] = {1};
  }
  TintaReceiveCheckpoint received;
  received.course = bindings.course;
  received.storage = bindings.storage;
  received.owner.fill(9);
  received.transaction.fill(8);
  ASSERT_NE(SHA256(expected.data(), expected.size(), received.manifestHash.data()), nullptr);
  received.length = manifest.length(TintaDerivedFile::Items);
  received.sequence = 1;
  HalTintaReceiveJournalStorage receivedStorage(bindings.course);
  std::array<uint8_t, TINTA_RECEIVE_CHECKPOINT_SIZE> receiveScratch{};
  TintaReceiveJournal receiveJournal(receivedStorage, receiveScratch);
  ASSERT_EQ(receiveJournal.open(), TintaReceiveJournalResult::Missing);
  ASSERT_EQ(receiveJournal.start(received), TintaReceiveJournalResult::Ok);
  for (unsigned i = 0; i < 5; ++i) {
    SCOPED_TRACE(i);
    const auto kind = static_cast<TintaDerivedFile>(i);
    std::optional<HalTintaDerivedCandidateStage> reception;
    reception.emplace(scratch);
    ASSERT_TRUE(reception->begin(manifest, kind, bindings.course, bindings.storage, bindings.pack, bindings.frontier));
    for (size_t at = 0; at < candidates[i].size();) {
      const auto count = std::min(size_t{37}, candidates[i].size() - at);
      ASSERT_TRUE(reception->write(at, std::span(candidates[i]).subspan(at, count)));
      at += count;
      uint64_t durable = 0;
      ASSERT_TRUE(reception->syncPending(durable));
      ASSERT_EQ(durable, at);
      received.offset = durable;
      ++received.sequence;
      ASSERT_EQ(receiveJournal.advance(received), TintaReceiveJournalResult::Ok);
      if (i == 0 && at == 37) {
        ASSERT_TRUE(reception->retainPending(durable));
        reception.reset();
        ASSERT_EQ(receiveJournal.open(), TintaReceiveJournalResult::Ok);
        ASSERT_NE(receiveJournal.checkpoint(), nullptr);
        const auto recovered = *receiveJournal.checkpoint();
        ASSERT_EQ(recovered.offset, durable);
        ASSERT_EQ(recovered.sequence, received.sequence);
        reception.emplace(scratch);
        ASSERT_TRUE(reception->resume(recovered, manifest, received.manifestHash, received.transaction, received.owner,
                                      bindings.course, bindings.storage, bindings.pack, bindings.frontier));
      }
      for (unsigned active = 0; active < 5; ++active) {
        ASSERT_TRUE(tintaDerivedFilePath(bindings.course, static_cast<TintaDerivedFile>(active),
                                         TintaDerivedRole::Active, path));
        ASSERT_EQ(state.files[path.data()], (std::vector<uint8_t>{1}));
      }
    }
    ASSERT_TRUE(reception->seal());
    ASSERT_TRUE(reception->isSealed());
    ASSERT_TRUE(tintaDerivedFilePath(bindings.course, kind, TintaDerivedRole::Candidate, path));
    ASSERT_EQ(state.files[path.data()], candidates[i]);
    if (i < 4) {
      received.sealedMask |= static_cast<uint8_t>(1u << i);
      received.file = static_cast<TintaDerivedFile>(i + 1);
      received.length = manifest.length(received.file);
      received.offset = 0;
      ++received.sequence;
      ASSERT_EQ(receiveJournal.advance(received), TintaReceiveJournalResult::Ok);
    }
  }
  {
    HalTintaDerivedPublicationStorage backend(bindings, expected, scratch);
    TintaDerivedPublication publication(backend);
    const auto before = state.files;
    EXPECT_FALSE(backend.persistIntent(expected));
    EXPECT_EQ(publication.publish(expected), TintaPublicationResult::Invalid);
    EXPECT_EQ(state.files, before);
  }
  bindings.proveJournal = [](void*, const TintaDerivedManifestView&) { return true; };
  const auto initial = state;
  for (unsigned cut = 0; cut <= 15; ++cut)
    for (bool after : {false, true}) {
      if ((!cut || cut > 12) && after) continue;
      SCOPED_TRACE(testing::Message() << "publication fault " << cut << ", after effect " << after);
      state = initial;
      state.failRename = !after && cut <= 12 ? cut : 0;
      state.failRenameAfter = after && cut <= 12 ? cut : 0;
      state.failSync = cut == 13;
      state.failRemove = cut == 14;
      state.failRemoveAfter = cut == 15;
      {
        HalTintaDerivedPublicationStorage backend(bindings, expected, scratch);
        TintaDerivedPublication publication(backend);
        EXPECT_EQ(publication.publish(expected), cut ? TintaPublicationResult::IoError : TintaPublicationResult::Ok);
      }
      state.failRename = state.failRenameAfter = 0;
      state.failSync = state.failRemove = state.failRemoveAfter = false;
      std::array<uint8_t, TINTA_DERIVED_MANIFEST_SIZE> pending{}, previous{};
      HalTintaDerivedRecordReader recoveryReader(bindings.course, scratch);
      const auto recovered = recoverTintaDerivedPublication(bindings, recoveryReader, pending, previous, scratch);
      EXPECT_EQ(recovered, cut && (cut > 1 || after) && cut != 13 ? TintaDerivedRecoveryResult::Recovered
                                                                  : TintaDerivedRecoveryResult::NoPending);
      HalTintaDerivedPublicationStorage restarted(bindings, expected, scratch);
      TintaDerivedPublication publication(restarted);
      ASSERT_EQ(publication.publish(expected), TintaPublicationResult::Ok);
      for (unsigned i = 0; i < 5; ++i) {
        ASSERT_TRUE(
            tintaDerivedFilePath(bindings.course, static_cast<TintaDerivedFile>(i), TintaDerivedRole::Active, path));
        EXPECT_EQ(state.files[path.data()], candidates[i]);
        for (auto role : {TintaDerivedRole::Candidate, TintaDerivedRole::Backup}) {
          ASSERT_TRUE(tintaDerivedFilePath(bindings.course, static_cast<TintaDerivedFile>(i), role, path));
          EXPECT_FALSE(state.files.contains(path.data()));
        }
      }
      const auto before = state.files;
      EXPECT_EQ(publication.publish(expected), TintaPublicationResult::Ok);
      EXPECT_EQ(state.files, before);
    }
}
TEST_F(InventoryHashTest, TintaDataRenamesRequireIntentAndBackupRemovalRequiresCommit) {
  std::ifstream input(TINTA_DERIVED_FIXTURE, std::ios::binary);
  ASSERT_TRUE(input.good());
  const std::vector<uint8_t> expected{std::istreambuf_iterator<char>(input), {}};
  Identity course{};
  course.fill(7);
  std::array<char, COURSE_STATE_DIRECTORY_SIZE> root{};
  std::array<char, COURSE_STATE_PATH_SIZE> path{};
  ASSERT_TRUE(courseStateDirectory(course, root));
  auto& state = inventory_hal_test::state;
  state.directories[root.data()] = {};
  state.enumerateFileMap = true;
  for (auto role : {TintaDerivedRole::Active, TintaDerivedRole::Candidate}) {
    ASSERT_TRUE(tintaDerivedFilePath(course, TintaDerivedFile::Days, role, path));
    state.files[path.data()] = {'T', 'D', 'L', role == TintaDerivedRole::Active ? uint8_t('0') : uint8_t('1')};
  }
  std::array<uint8_t, 512> scratch{};
  HalTintaDerivedRecordReader reader(course, scratch);
  HalTintaDerivedFileOwnership files(course, expected, reader, scratch);
  EXPECT_EQ(files.state(TintaDerivedFile::Days, TintaDerivedRole::Candidate), TintaPublicationState::Matches);
  const auto before = state.files;
  EXPECT_FALSE(files.rename(TintaDerivedFile::Days, TintaDerivedRole::Active, TintaDerivedRole::Backup));
  EXPECT_EQ(state.files, before);
  ASSERT_TRUE(tintaDerivedRecordPath(course, TintaDerivedRecord::Intent, path));
  state.files[path.data()] = expected;
  ASSERT_TRUE(files.rename(TintaDerivedFile::Days, TintaDerivedRole::Active, TintaDerivedRole::Backup));
  ASSERT_TRUE(files.rename(TintaDerivedFile::Days, TintaDerivedRole::Candidate, TintaDerivedRole::Active));
  EXPECT_EQ(files.state(TintaDerivedFile::Days, TintaDerivedRole::Active), TintaPublicationState::Matches);
  EXPECT_FALSE(files.remove(TintaDerivedFile::Days, TintaDerivedRole::Backup));
  ASSERT_TRUE(tintaDerivedRecordPath(course, TintaDerivedRecord::Receipt, path));
  state.files[path.data()] = expected;
  ASSERT_TRUE(files.remove(TintaDerivedFile::Days, TintaDerivedRole::Backup));
  EXPECT_TRUE(files.remove(TintaDerivedFile::Days, TintaDerivedRole::Backup));
  EXPECT_FALSE(files.remove(TintaDerivedFile::Days, TintaDerivedRole::Active));
}
TEST_F(InventoryHashTest, TintaReceiptRejectsMissingIntentAndNonIncreasingRevision) {
  std::ifstream input(TINTA_DERIVED_FIXTURE, std::ios::binary);
  ASSERT_TRUE(input.good());
  const std::vector<uint8_t> previous{std::istreambuf_iterator<char>(input), {}};
  auto expected = previous;
  expected[84] ^= 1;
  tinta::core::putU32(expected.data() + 328, tinta::core::crc32(expected.data(), 328));
  Identity course{};
  course.fill(7);
  std::array<char, COURSE_STATE_DIRECTORY_SIZE> root{};
  std::array<char, COURSE_STATE_PATH_SIZE> receipt{}, intent{};
  ASSERT_TRUE(courseStateDirectory(course, root));
  ASSERT_TRUE(tintaDerivedRecordPath(course, TintaDerivedRecord::Receipt, receipt));
  ASSERT_TRUE(tintaDerivedRecordPath(course, TintaDerivedRecord::Intent, intent));
  auto& state = inventory_hal_test::state;
  state.directories[root.data()] = {};
  state.enumerateFileMap = true;
  state.files[receipt.data()] = previous;
  std::array<uint8_t, 512> scratch{};
  HalTintaDerivedRecordReader reader(course, scratch);
  HalTintaDerivedReceiptWriter writer(course, reader);
  const auto before = state.files;
  EXPECT_FALSE(writer.commit(expected, previous));
  EXPECT_EQ(state.files, before);
  state.files[intent.data()] = expected;
  const auto withIntent = state.files;
  EXPECT_FALSE(writer.commit(expected, previous));
  EXPECT_EQ(state.files, withIntent);
  EXPECT_FALSE(writer.clearIntent(expected));
}
TEST_F(InventoryHashTest, TintaReceiptReplacementRecoversSyncRemoveAndRenameFailures) {
  std::ifstream input(TINTA_DERIVED_FIXTURE, std::ios::binary);
  ASSERT_TRUE(input.good());
  const std::vector<uint8_t> previous{std::istreambuf_iterator<char>(input), {}};
  auto expected = previous;
  expected[120] = 2;
  tinta::core::putU32(expected.data() + 328, tinta::core::crc32(expected.data(), 328));
  Identity course{};
  course.fill(7);
  std::array<char, COURSE_STATE_DIRECTORY_SIZE> root{};
  std::array<char, COURSE_STATE_PATH_SIZE> receipt{}, intent{};
  ASSERT_TRUE(courseStateDirectory(course, root));
  ASSERT_TRUE(tintaDerivedRecordPath(course, TintaDerivedRecord::Receipt, receipt));
  ASSERT_TRUE(tintaDerivedRecordPath(course, TintaDerivedRecord::Intent, intent));
  for (unsigned failure = 0; failure < 4; ++failure) {
    inventory_hal_test::state = {};
    auto& state = inventory_hal_test::state;
    state.directories[root.data()] = {};
    state.enumerateFileMap = true;
    state.files[receipt.data()] = previous;
    state.files[intent.data()] = expected;
    if (failure == 0) state.failSync = true;
    if (failure == 1) state.failRename = 1;
    if (failure == 2) state.failRenameAfter = 1;
    if (failure == 3) state.failRemoveAfter = true;
    std::array<uint8_t, 512> scratch{};
    HalTintaDerivedRecordReader reader(course, scratch);
    HalTintaDerivedReceiptWriter writer(course, reader);
    EXPECT_FALSE(writer.commit(expected, previous));
    state.failSync = false;
    state.failRename = 0;
    state.failRenameAfter = 0;
    state.failRemoveAfter = false;
    ASSERT_TRUE(writer.commit(expected, previous));
    EXPECT_EQ(state.files[receipt.data()], expected);
    const auto files = state.files;
    ASSERT_TRUE(writer.commit(expected));
    EXPECT_EQ(state.files, files);
    state.failRemoveAfter = true;
    EXPECT_FALSE(writer.clearIntent(expected));
    state.failRemoveAfter = false;
    EXPECT_TRUE(writer.clearIntent(expected));
    EXPECT_EQ(reader.inspect(TintaDerivedRecord::Intent, expected), TintaPublicationState::Missing);
    EXPECT_TRUE(writer.commit(expected));
  }
}
TEST_F(InventoryHashTest, TintaIntentPublicationRetriesSyncReadbackAndLostRenameAcknowledgments) {
  std::ifstream input(TINTA_DERIVED_FIXTURE, std::ios::binary);
  ASSERT_TRUE(input.good());
  const std::vector<uint8_t> expected{std::istreambuf_iterator<char>(input), {}};
  Identity course{};
  course.fill(7);
  std::array<char, COURSE_STATE_DIRECTORY_SIZE> root{};
  ASSERT_TRUE(courseStateDirectory(course, root));
  for (unsigned failure = 0; failure < 4; ++failure) {
    inventory_hal_test::state = {};
    auto& state = inventory_hal_test::state;
    state.directories[root.data()] = {};
    state.enumerateFileMap = true;
    if (failure == 0) state.failSync = true;
    if (failure == 1) state.failRead = 1;
    if (failure == 2) state.failRename = 1;
    if (failure == 3) state.failRenameAfter = 1;
    std::array<uint8_t, 512> scratch{};
    HalTintaDerivedRecordReader reader(course, scratch);
    HalTintaDerivedIntentWriter writer(course, reader);
    EXPECT_FALSE(writer.persist(expected));
    state.failSync = false;
    state.failRead = 0;
    state.failRename = 0;
    state.failRenameAfter = 0;
    ASSERT_TRUE(writer.persist(expected));
    EXPECT_EQ(reader.inspect(TintaDerivedRecord::Intent, expected), TintaPublicationState::Matches);
    const auto files = state.files;
    const auto renames = state.renames;
    EXPECT_TRUE(writer.persist(expected));
    EXPECT_EQ(state.files, files);
    EXPECT_EQ(state.renames, renames);
    Identity foreign{};
    foreign.fill(8);
    HalTintaDerivedIntentWriter wrongCourse(foreign, reader);
    EXPECT_FALSE(wrongCourse.persist(expected));
    EXPECT_EQ(state.files, files);
  }
}
TEST_F(InventoryHashTest, TintaRecoveryRecordLookupDistinguishesMissingConflictAndIoFailure) {
  std::ifstream input(TINTA_DERIVED_FIXTURE, std::ios::binary);
  ASSERT_TRUE(input.good());
  const std::vector<uint8_t> expected{std::istreambuf_iterator<char>(input), {}};
  Identity course{};
  course.fill(7);
  std::array<char, COURSE_STATE_DIRECTORY_SIZE> root{};
  std::array<char, COURSE_STATE_PATH_SIZE> path{};
  ASSERT_TRUE(courseStateDirectory(course, root));
  ASSERT_TRUE(tintaDerivedRecordPath(course, TintaDerivedRecord::Intent, path));
  auto& state = inventory_hal_test::state;
  state.directories[root.data()] = {};
  state.enumerateFileMap = true;
  std::array<uint8_t, 512> scratch{};
  HalTintaDerivedRecordReader reader(course, scratch);
  EXPECT_EQ(reader.inspect(TintaDerivedRecord::Intent, expected), TintaPublicationState::Missing);
  state.files[path.data()] = expected;
  EXPECT_EQ(reader.inspect(TintaDerivedRecord::Intent, expected), TintaPublicationState::Matches);
  state.files[path.data()][0] ^= 1;
  EXPECT_EQ(reader.inspect(TintaDerivedRecord::Intent, expected), TintaPublicationState::Other);
  state.files[path.data()] = expected;
  state.failRead = state.reads + 1;
  EXPECT_EQ(reader.inspect(TintaDerivedRecord::Intent, expected), TintaPublicationState::Error);
  state.failRead = 0;
  EXPECT_EQ(reader.inspect(TintaDerivedRecord::Intent, expected), TintaPublicationState::Matches);
  state.directoryErrorPath = root.data();
  EXPECT_EQ(reader.inspect(TintaDerivedRecord::Intent, expected), TintaPublicationState::Error);
}
TEST_F(InventoryHashTest, GenerationRequiresAllFiveFilesAndRestartsAfterFailure) {
  std::ifstream input(TINTA_DERIVED_FIXTURE, std::ios::binary);
  ASSERT_TRUE(input.good());
  const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
  TintaDerivedManifestView manifest;
  ASSERT_TRUE(manifest.decode(bytes));
  Identity course{}, storage{};
  Digest pack{}, frontier{};
  course.fill(7);
  storage.fill(1);
  pack.fill(3);
  frontier.fill(4);
  auto& state = inventory_hal_test::state;
  std::string itemPath(TINTA_DERIVED_FIXTURE);
  itemPath.replace(itemPath.find("TintaDerivedManifest-v1.fixture"), std::string::npos, "TintaItemSnapshot-v1.fixture");
  std::ifstream items(itemPath, std::ios::binary);
  ASSERT_TRUE(items.good());
  state.files["/items"] = {std::istreambuf_iterator<char>(items), {}};
  state.files["/reviews"] = {};
  state.files["/days"] = {'T', 'D', 'L', '1'};
  for (unsigned kind = 1; kind <= 2; ++kind) {
    auto& file = state.files[kind == 1 ? "/lessons" : "/readings"];
    file.resize(16);
    std::memcpy(file.data(), "TCS1", 4);
    file[4] = kind;
    tinta::core::putU32(file.data() + 12, tinta::core::crc32(file.data(), 12));
  }
  constexpr const char* paths[] = {"/items", "/reviews", "/lessons", "/readings", "/days"};
  HalTintaDerivedGenerationValidation validation;
  std::array<uint8_t, 128> scratch{};
  ASSERT_TRUE(validation.begin(manifest, course, storage, pack, frontier));
  for (unsigned i = 0; i < 5; ++i) {
    EXPECT_FALSE(validation.complete());
    HalFile file(paths[i]);
    ASSERT_TRUE(validation.file(static_cast<TintaDerivedFile>(i), file, scratch));
  }
  EXPECT_TRUE(validation.complete());
  state.files["/days"][3] ^= 1;
  HalFile days("/days");
  EXPECT_FALSE(validation.file(TintaDerivedFile::Days, days, scratch));
  EXPECT_FALSE(validation.complete());
  state.files["/days"][3] ^= 1;
  EXPECT_FALSE(validation.file(TintaDerivedFile::Days, days, scratch));
  ASSERT_TRUE(validation.begin(manifest, course, storage, pack, frontier));
  ASSERT_TRUE(validation.file(TintaDerivedFile::Days, days, scratch));
  EXPECT_FALSE(validation.complete());
  frontier[0] ^= 1;
  EXPECT_FALSE(validation.begin(manifest, course, storage, pack, frontier));
  frontier[0] ^= 1;
  auto malformedReceipt = bytes;
  state.files["/days"][3] = '0';
  const auto& malformedFile = state.files["/days"];
  ASSERT_EQ(EVP_Digest(malformedFile.data(), malformedFile.size(), malformedReceipt.data() + 296, nullptr, EVP_sha256(),
                       nullptr),
            1);
  tinta::core::putU32(malformedReceipt.data() + 328, tinta::core::crc32(malformedReceipt.data(), 328));
  TintaDerivedManifestView malformedManifest;
  ASSERT_TRUE(malformedManifest.decode(malformedReceipt));
  ASSERT_TRUE(validation.begin(malformedManifest, course, storage, pack, frontier));
  EXPECT_FALSE(validation.file(TintaDerivedFile::Days, days, scratch));
  EXPECT_FALSE(validation.complete());
}
TEST_F(InventoryHashTest, CanonicalItemSnapshotChecksBothHeadersRecordsAndReadFailures) {
  std::string path(TINTA_DERIVED_FIXTURE);
  path.replace(path.find("TintaDerivedManifest-v1.fixture"), std::string::npos, "TintaItemSnapshot-v1.fixture");
  std::ifstream input(path, std::ios::binary);
  ASSERT_TRUE(input.good());
  const std::vector<uint8_t> original{std::istreambuf_iterator<char>(input), {}};
  auto& state = inventory_hal_test::state;
  state.files["/items"] = original;
  HalFile file("/items");
  std::array<uint8_t, 80> scratch{};
  ASSERT_TRUE(validateTintaItemSnapshot(file, 42, scratch));
  EXPECT_FALSE(validateTintaItemSnapshot(file, 41, scratch));
  for (unsigned fault = 0; fault < 5; ++fault) {
    auto& bytes = state.files["/items"];
    bytes = original;
    if (fault == 0) bytes[588] ^= 1;
    if (fault == 1) {
      bytes[538] = 1;
      tinta::core::putU32(bytes.data() + 588, tinta::core::crc32(bytes.data() + 512, 76));
    }
    if (fault == 2) tinta::core::putU32(bytes.data() + 1040, 1000);
    if (fault == 3) bytes.push_back(0);
    if (fault == 4) state.failRead = state.reads + 3;
    EXPECT_FALSE(validateTintaItemSnapshot(file, 42, scratch));
    state.failRead = 0;
  }
  state.files["/items"] = original;
  EXPECT_TRUE(validateTintaItemSnapshot(file, 42, scratch));
  EXPECT_TRUE(file.isOpen());
}
TEST_F(InventoryHashTest, TintaManifestReceiptsVerifyActualBytesAndRejectMismatchOrReadError) {
  std::ifstream input(TINTA_DERIVED_FIXTURE, std::ios::binary);
  ASSERT_TRUE(input.good());
  const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
  TintaDerivedManifestView manifest;
  ASSERT_TRUE(manifest.decode(bytes));
  auto& state = inventory_hal_test::state;
  state.files["/days"] = {'T', 'D', 'L', '1'};
  state.files["/reviews"] = {};
  HalFile file("/days"), empty("/reviews");
  std::array<uint8_t, 2> scratch{};
  EXPECT_EQ(verifyTintaDerivedFileReceipt(file, manifest, TintaDerivedFile::Days, scratch),
            TintaDerivedVerification::Verified);
  EXPECT_EQ(verifyTintaDerivedFileReceipt(empty, manifest, TintaDerivedFile::LocalReviews, scratch),
            TintaDerivedVerification::Verified);
  state.files["/days"][3] ^= 1;
  EXPECT_EQ(verifyTintaDerivedFileReceipt(file, manifest, TintaDerivedFile::Days, scratch),
            TintaDerivedVerification::Conflict);
  state.files["/days"][3] ^= 1;
  state.failRead = state.reads + 1;
  EXPECT_EQ(verifyTintaDerivedFileReceipt(file, manifest, TintaDerivedFile::Days, scratch),
            TintaDerivedVerification::IoError);
  state.failRead = 0;
  EXPECT_EQ(verifyTintaDerivedFileReceipt(file, manifest, TintaDerivedFile::Days, scratch),
            TintaDerivedVerification::Verified);
  state.files["/days"].push_back(0);
  const auto before = state.reads;
  EXPECT_EQ(verifyTintaDerivedFileReceipt(file, manifest, TintaDerivedFile::Days, scratch),
            TintaDerivedVerification::Conflict);
  EXPECT_EQ(state.reads, before);
  EXPECT_TRUE(file.isOpen());
}
TEST_F(InventoryHashTest, StreamsBorrowedFileFromStartWithoutReopening) {
  auto& state = inventory_hal_test::state;
  auto& bytes = state.files["/book"];
  bytes.resize(10001);
  for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = i % 251;
  HalFile file("/book");
  ASSERT_TRUE(file.seek64(200));
  std::array<uint8_t, 127> scratch;
  Digest actual{}, expected{};
  uint64_t length = 0;
  ASSERT_EQ(EVP_Digest(bytes.data(), bytes.size(), expected.data(), nullptr, EVP_sha256(), nullptr), 1);
  ASSERT_TRUE(hashInventoryFile(file, scratch, length, actual));
  EXPECT_EQ(actual, expected);
  EXPECT_EQ(length, bytes.size());
  EXPECT_EQ(state.opens, 0u);
  EXPECT_EQ(state.yields, 79u);
  EXPECT_TRUE(file);
}
TEST_F(InventoryHashTest, EmptyFileHasStandardDigest) {
  inventory_hal_test::state.files["/empty"] = {};
  HalFile file("/empty");
  std::array<uint8_t, 1> scratch;
  Digest actual{}, expected{};
  uint64_t length = 3;
  ASSERT_EQ(EVP_Digest("", 0, expected.data(), nullptr, EVP_sha256(), nullptr), 1);
  ASSERT_TRUE(hashInventoryFile(file, scratch, length, actual));
  EXPECT_EQ(length, 0u);
  EXPECT_EQ(actual, expected);
}
TEST_F(InventoryHashTest, CancellationPreservesOutputsAndRetryUsesSameBorrowedHandle) {
  inventory_hal_test::state.files["/book"].resize(1000);
  HalFile file("/book");
  std::array<uint8_t, 127> scratch;
  Digest hash{};
  hash.fill(42);
  const auto original = hash;
  uint64_t length = 42;
  unsigned calls = 0;
  EXPECT_FALSE(hashInventoryFile(
      file, scratch, length, hash, [](void* context) { return ++*static_cast<unsigned*>(context) < 2; }, &calls));
  EXPECT_EQ(length, 42u);
  EXPECT_EQ(hash, original);
  EXPECT_TRUE(file);
  EXPECT_TRUE(hashInventoryFile(file, scratch, length, hash));
  EXPECT_EQ(length, 1000u);
}
TEST_F(InventoryHashTest, ShortAndFailedReadsDoNotPublishOutputs) {
  for (unsigned failure = 0; failure < 2; ++failure) {
    auto& state = inventory_hal_test::state;
    state = {};
    state.files["/book"].resize(1000);
    if (failure == 0)
      state.shortRead = 2;
    else
      state.failRead = 2;
    HalFile file("/book");
    std::array<uint8_t, 127> scratch;
    Digest actual{};
    actual.fill(42);
    const auto original = actual;
    uint64_t length = 42;
    EXPECT_FALSE(hashInventoryFile(file, scratch, length, actual));
    EXPECT_EQ(length, 42u);
    EXPECT_EQ(actual, original);
    EXPECT_GT(state.errors, 0u);
  }
}
TEST_F(InventoryHashTest, InvalidHandleAndEmptyScratchFail) {
  HalFile invalid;
  std::array<uint8_t, 1> scratch;
  Digest hash{};
  uint64_t length = 7;
  EXPECT_FALSE(hashInventoryFile(invalid, scratch, length, hash));
  inventory_hal_test::state.files["/book"] = {1};
  HalFile file("/book");
  EXPECT_FALSE(hashInventoryFile(file, {}, length, hash));
  EXPECT_EQ(length, 7u);
}

TEST_F(InventoryHashTest, ReaderFontProofChecksInstalledBytesFamilySizeAndReadFailuresWithoutWrites) {
  std::ifstream input(READER_FONT_FIXTURE, std::ios::binary);
  const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  ASSERT_FALSE(bytes.empty());
  const char* path = "/.fonts/Test/Test_14.cpfont";
  auto& state = inventory_hal_test::state;
  state.files[path] = bytes;
  const auto original = state.files;
  Digest hash;
  unsigned length = 0;
  ASSERT_EQ(EVP_Digest(bytes.data(), bytes.size(), hash.data(), &length, EVP_sha256(), nullptr), 1);
  ASSERT_EQ(length, hash.size());
  std::array<uint8_t, 128> scratch{};
  HalReaderPreferenceFontFileProof proof(scratch);
  const std::array<char, 4> family{'T', 'e', 's', 't'};
  EXPECT_EQ(proof.contentHash(), nullptr);
  ASSERT_TRUE(proof.verify(path, family, 14, {}, false));
  ASSERT_NE(proof.contentHash(), nullptr);
  EXPECT_EQ(*proof.contentHash(), hash);
  ASSERT_TRUE(proof.verify(path, family, 14, hash, false));
  EXPECT_FALSE(proof.verify(path, family, 12, hash, false));
  EXPECT_EQ(proof.contentHash(), nullptr);
  auto wrong = hash;
  wrong[0] ^= 1;
  EXPECT_FALSE(proof.verify(path, family, 14, wrong, false));
  EXPECT_EQ(proof.contentHash(), nullptr);
  EXPECT_FALSE(proof.verify(path, std::span(family).first(3), 14, hash, false));
  state.failRead = state.reads + 1;
  EXPECT_FALSE(proof.verify(path, family, 14, hash, false));
  state.failRead = 0;
  state.failClose = true;
  EXPECT_FALSE(proof.verify(path, family, 14, hash, false));
  state.failClose = false;
  EXPECT_EQ(proof.contentHash(), nullptr);
  state.files[path][0] ^= 1;
  EXPECT_FALSE(proof.verify(path, family, 14, hash, false));
  state.files[path][0] ^= 1;
  ASSERT_TRUE(proof.verify(path, family, 14, hash, false));
  ASSERT_NE(proof.contentHash(), nullptr);
  EXPECT_EQ(*proof.contentHash(), hash);
  EXPECT_EQ(state.files, original);
}

TEST_F(InventoryHashTest, ReaderVectorFontProofRequiresSupportAndExactFileHashWithoutWrites) {
  std::ifstream input(READER_VECTOR_FONT_FIXTURE, std::ios::binary);
  const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  ASSERT_FALSE(bytes.empty());
  const char* path = "/fonts/Test.TTF";
  auto& state = inventory_hal_test::state;
  state.files[path] = bytes;
  const auto original = state.files;
  Digest hash;
  unsigned length = 0;
  ASSERT_EQ(EVP_Digest(bytes.data(), bytes.size(), hash.data(), &length, EVP_sha256(), nullptr), 1);
  ASSERT_EQ(length, hash.size());
  std::array<uint8_t, 128> scratch{};
  HalReaderPreferenceFontFileProof proof(scratch);
  const std::array<char, 4> family{'T', 'e', 's', 't'};
  EXPECT_FALSE(proof.verify(path, family, 14, hash, false));
  ASSERT_TRUE(proof.verify(path, family, 14, hash, true));
  auto wrong = hash;
  wrong[0] ^= 1;
  EXPECT_FALSE(proof.verify(path, family, 14, wrong, true));
  EXPECT_FALSE(proof.verify(path, std::span(family).first(3), 14, hash, true));
  state.failClose = true;
  EXPECT_FALSE(proof.verify(path, family, 14, hash, true));
  state.failClose = false;
  ASSERT_TRUE(proof.verify(path, family, 14, hash, true));
  EXPECT_EQ(state.files, original);
}

TEST_F(InventoryHashTest, DictionaryHashStageMatchesStreamHashAndClearsFailedOrCancelledResults) {
  const auto original = inventory_hal_test::state.files;
  const std::array<uint8_t, 32> bytes{1, 2, 3, 4};
  Digest expected;
  unsigned length = 0;
  ASSERT_EQ(EVP_Digest(bytes.data(), bytes.size(), expected.data(), &length, EVP_sha256(), nullptr), 1);
  bool allowed = true;
  HalDictionaryBundleHashStage stage([](void* context) { return *static_cast<bool*>(context); }, &allowed);
  EXPECT_EQ(stage.contentHash(), nullptr);
  ASSERT_TRUE(stage.begin());
  ASSERT_TRUE(stage.write(0, std::span(bytes).first(13)));
  ASSERT_TRUE(stage.write(13, std::span(bytes).subspan(13)));
  ASSERT_TRUE(stage.seal(bytes.size()));
  ASSERT_NE(stage.contentHash(), nullptr);
  EXPECT_EQ(*stage.contentHash(), expected);
  EXPECT_FALSE(stage.write(bytes.size(), bytes));
  EXPECT_EQ(stage.contentHash(), nullptr);
  ASSERT_TRUE(stage.begin());
  EXPECT_FALSE(stage.write(1, bytes));
  EXPECT_EQ(stage.contentHash(), nullptr);
  ASSERT_TRUE(stage.begin());
  ASSERT_TRUE(stage.write(0, bytes));
  EXPECT_FALSE(stage.seal(bytes.size() + 1));
  EXPECT_EQ(stage.contentHash(), nullptr);
  ASSERT_TRUE(stage.begin());
  allowed = false;
  EXPECT_FALSE(stage.write(0, bytes));
  EXPECT_EQ(stage.contentHash(), nullptr);
  EXPECT_FALSE(stage.begin());
  allowed = true;
  ASSERT_TRUE(stage.begin());
  ASSERT_TRUE(stage.write(0, bytes));
  ASSERT_TRUE(stage.seal(bytes.size()));
  ASSERT_NE(stage.contentHash(), nullptr);
  EXPECT_EQ(*stage.contentHash(), expected);
  EXPECT_EQ(inventory_hal_test::state.files, original);
}

TEST_F(InventoryHashTest, CanonicalDictionaryBuilderProducesSameHashWithoutCreatingAnArchiveFile) {
  class Source final : public DictionaryBundleSource {
   public:
    bool size(unsigned member, uint64_t& bytes) override {
      if (member >= 3) return false;
      bytes = 5 + member;
      return true;
    }
    bool read(unsigned member, uint64_t offset, std::span<uint8_t> bytes) override {
      if (member >= 3 || offset > 5 + member || bytes.size() > 5 + member - offset) return false;
      std::fill(bytes.begin(), bytes.end(), static_cast<uint8_t>(member + 1));
      return true;
    }
    bool close() override { return true; }
  } source;
  class Stage final : public DictionaryBundleStage {
   public:
    std::vector<uint8_t> bytes;
    Stage() { bytes.reserve(512); }
    bool begin() override {
      bytes.clear();
      return true;
    }
    bool write(uint64_t offset, std::span<const uint8_t> input) override {
      if (offset != bytes.size()) return false;
      bytes.insert(bytes.end(), input.begin(), input.end());
      return true;
    }
    bool seal(uint64_t size) override { return size == bytes.size(); }
    void abort() override { bytes.clear(); }
  } reference;
  const auto original = inventory_hal_test::state.files;
  std::array<uint8_t, 64> scratch{};
  uint64_t expectedLength = 0, actualLength = 0;
  DictionaryBundleBuilder referenceBuilder(reference, scratch);
  ASSERT_TRUE(referenceBuilder.build(source, false, false, expectedLength));
  Digest expected;
  unsigned length = 0;
  ASSERT_EQ(EVP_Digest(reference.bytes.data(), reference.bytes.size(), expected.data(), &length, EVP_sha256(), nullptr),
            1);
  HalDictionaryBundleHashStage stage;
  DictionaryBundleBuilder builder(stage, scratch);
  ASSERT_TRUE(builder.build(source, false, false, actualLength));
  ASSERT_NE(stage.contentHash(), nullptr);
  EXPECT_EQ(*stage.contentHash(), expected);
  EXPECT_EQ(actualLength, expectedLength);
  EXPECT_EQ(inventory_hal_test::state.files, original);
}
