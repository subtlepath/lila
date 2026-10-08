#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "lib/Companion/CompanionDictionaryExtractionJournal.h"
#include "lib/Companion/CompanionDictionaryExtractionParent.h"
using namespace companion;
namespace {
struct StorageFake final : DictionaryExtractionJournalStorage {
  std::map<std::string, std::vector<uint8_t>> files;
  bool failRead = false, failWrite = false, afterEffect = false, corruptWrite = false, failStat = false;
  unsigned writes = 0;
  bool prepare() override { return true; }
  FileStatus stat(const char* path, uint64_t& length) override {
    if (failStat) return FileStatus::Error;
    if (!files.contains(path)) return FileStatus::Missing;
    length = files.at(path).size();
    return FileStatus::Present;
  }
  bool read(const char* path, uint64_t at, std::span<uint8_t> bytes) override {
    if (failRead || !files.contains(path) || at > files.at(path).size() || bytes.size() > files.at(path).size() - at)
      return false;
    std::copy_n(files.at(path).begin() + at, bytes.size(), bytes.begin());
    return true;
  }
  bool write(const char* path, uint64_t at, std::span<const uint8_t> bytes, bool truncate) override {
    ++writes;
    if (failWrite && !afterEffect) return false;
    if (at != 0 || !truncate) return false;
    files[path] = {bytes.begin(), bytes.end()};
    if (corruptWrite) files[path][0] ^= 1;
    return !failWrite;
  }
};
class DictionaryExtractionJournalTest : public testing::Test {
 protected:
  StorageFake storage;
  std::array<uint8_t, DICTIONARY_EXTRACTION_RECEIPT_SIZE> scratch{};
  DictionaryExtractionReceipt initial;
  Digest hash{};
  void SetUp() override {
    initial.revision = 1;
    initial.transaction[0] = 1;
    initial.generation[0] = 2;
    initial.archiveHash[0] = 3;
    initial.lengths = {6, 24, 111, 10};
    initial.synonyms = true;
    hash[0] = 4;
  }
  void put(unsigned slot, const DictionaryExtractionReceipt& receipt) {
    ASSERT_EQ(encodeDictionaryExtractionReceipt(receipt, scratch), scratch.size());
    storage.files[DICTIONARY_EXTRACTION_JOURNALS[slot]] = {scratch.begin(), scratch.end()};
  }
};
TEST_F(DictionaryExtractionJournalTest, PersistRecoverAndDuplicateReceiptsAvoidRedundantWrites) {
  DictionaryExtractionJournal journal(storage, scratch);
  ASSERT_EQ(journal.begin(initial), DictionaryJournalResult::Ok);
  for (unsigned member : {2u, 1u, 0u, 3u}) {
    ASSERT_EQ(journal.recordSealed(member, hash), DictionaryJournalResult::Ok);
    const auto writes = storage.writes;
    EXPECT_EQ(journal.recordSealed(member, hash), DictionaryJournalResult::Ok);
    EXPECT_EQ(storage.writes, writes);
    DictionaryExtractionJournal recovered(storage, scratch);
    ASSERT_EQ(recovered.recover(initial), DictionaryJournalResult::Ok);
    ASSERT_NE(recovered.current(), nullptr);
    EXPECT_EQ(*recovered.current(), *journal.current());
  }
  EXPECT_EQ(storage.writes, 5u);
  EXPECT_EQ(journal.current()->sealed, 15u);
  EXPECT_EQ(journal.begin(initial), DictionaryJournalResult::Conflict);
  EXPECT_EQ(journal.current(), nullptr);
}
TEST_F(DictionaryExtractionJournalTest, FailedWritesBeforeAndAfterEffectRequireRecovery) {
  for (bool after : {false, true}) {
    storage = {};
    DictionaryExtractionJournal journal(storage, scratch);
    ASSERT_EQ(journal.begin(initial), DictionaryJournalResult::Ok);
    storage.failWrite = true;
    storage.afterEffect = after;
    EXPECT_EQ(journal.recordSealed(2, hash), DictionaryJournalResult::IoError);
    EXPECT_EQ(journal.current(), nullptr);
    EXPECT_EQ(journal.recordSealed(1, hash), DictionaryJournalResult::Invalid);
    storage.failWrite = false;
    ASSERT_EQ(journal.recover(initial), DictionaryJournalResult::Ok);
    EXPECT_EQ(journal.current()->sealed, after ? 4u : 0u);
  }
}
TEST_F(DictionaryExtractionJournalTest, TornSlotFallsBackButUnreadableSlotFailsClosed) {
  DictionaryExtractionJournal journal(storage, scratch);
  ASSERT_EQ(journal.begin(initial), DictionaryJournalResult::Ok);
  storage.corruptWrite = true;
  EXPECT_EQ(journal.recordSealed(2, hash), DictionaryJournalResult::Corrupt);
  EXPECT_EQ(journal.current(), nullptr);
  storage.corruptWrite = false;
  ASSERT_EQ(journal.recover(initial), DictionaryJournalResult::Ok);
  EXPECT_EQ(journal.current()->sealed, 0u);
  storage.failRead = true;
  EXPECT_EQ(journal.recover(initial), DictionaryJournalResult::IoError);
  EXPECT_EQ(journal.current(), nullptr);
  storage.failRead = false;
  storage.failStat = true;
  EXPECT_EQ(journal.recover(initial), DictionaryJournalResult::IoError);
  EXPECT_EQ(journal.current(), nullptr);
}
TEST_F(DictionaryExtractionJournalTest, UnknownParentsAndConflictingRevisionsArePreserved) {
  for (unsigned fault = 0; fault < 5; ++fault) {
    storage = {};
    put(0, initial);
    auto other = initial;
    if (fault == 0) other.transaction[0] ^= 1;
    if (fault == 1) other.generation[0] ^= 1;
    if (fault == 2) other.archiveHash[0] ^= 1;
    if (fault == 3) ++other.lengths[0];
    if (fault == 4) other.compressed = true;
    // Keep identities nonzero when changing their only populated byte.
    other.transaction[1] = fault == 0;
    put(1, other);
    const auto unchanged = storage.files;
    DictionaryExtractionJournal journal(storage, scratch);
    EXPECT_EQ(journal.recover(initial), DictionaryJournalResult::Conflict) << fault;
    EXPECT_EQ(journal.current(), nullptr);
    EXPECT_EQ(storage.files, unchanged);
  }
  storage = {};
  put(0, initial);
  auto conflict = initial;
  conflict.sealed = 4;
  conflict.hashes[2] = hash;
  put(1, conflict);
  DictionaryExtractionJournal journal(storage, scratch);
  EXPECT_EQ(journal.recover(initial), DictionaryJournalResult::Corrupt);
  conflict.revision = 3;
  put(1, conflict);
  EXPECT_EQ(journal.recover(initial), DictionaryJournalResult::Corrupt);
}
TEST_F(DictionaryExtractionJournalTest, InvalidOrderAndForeignCorruptFilesCannotBeClaimed) {
  DictionaryExtractionJournal journal(storage, scratch);
  ASSERT_EQ(journal.begin(initial), DictionaryJournalResult::Ok);
  EXPECT_EQ(journal.recordSealed(0, hash), DictionaryJournalResult::Invalid);
  EXPECT_EQ(storage.writes, 1u);
  storage.files.clear();
  storage.files[DICTIONARY_EXTRACTION_JOURNALS[0]] = {1, 2, 3};
  const auto unchanged = storage.files;
  EXPECT_EQ(journal.begin(initial), DictionaryJournalResult::Corrupt);
  EXPECT_EQ(journal.current(), nullptr);
  EXPECT_EQ(storage.files, unchanged);
}
TEST_F(DictionaryExtractionJournalTest, EveryTruncatedReplacementRecoversPreviousOwnershipReceipt) {
  DictionaryExtractionJournal journal(storage, scratch);
  ASSERT_EQ(journal.begin(initial), DictionaryJournalResult::Ok);
  ASSERT_EQ(journal.recordSealed(2, hash), DictionaryJournalResult::Ok);
  const auto replacement = storage.files.at(DICTIONARY_EXTRACTION_JOURNALS[1]);
  for (size_t bytes = 0; bytes < replacement.size(); ++bytes) {
    storage.files[DICTIONARY_EXTRACTION_JOURNALS[1]] = {replacement.begin(), replacement.begin() + bytes};
    EXPECT_EQ(journal.recover(initial), DictionaryJournalResult::Ok) << bytes;
    ASSERT_NE(journal.current(), nullptr);
    EXPECT_EQ(*journal.current(), initial) << bytes;
  }
}
TEST_F(DictionaryExtractionJournalTest, ParentIdentityManifestAndIncompleteTransferRejectWithoutMutation) {
  TransferState parent;
  parent.transaction = initial.transaction;
  parent.storageGeneration = initial.generation;
  parent.owner[0] = 9;
  parent.contentHash = initial.archiveHash;
  parent.length = parent.durableOffset = 1000;
  ContentManifest manifest;
  manifest.kind = ContentKind::Dictionary;
  manifest.formatVersion = 1;
  manifest.length = parent.length;
  manifest.contentHash = parent.contentHash;
  for (unsigned fault = 0; fault < 10; ++fault) {
    storage = {};
    auto wrongParent = parent;
    auto wrongManifest = manifest;
    auto wrongGeneration = initial.generation;
    if (fault == 0) wrongParent.transaction[0] ^= 1;
    if (fault == 1) wrongParent.owner = {};
    if (fault == 2) wrongParent.storageGeneration[0] ^= 1;
    if (fault == 3) wrongGeneration[0] ^= 1;
    if (fault == 4) wrongParent.contentHash[0] ^= 1;
    if (fault == 5) --wrongParent.durableOffset;
    if (fault == 6) wrongParent.phase = TransferPhase::Aborted;
    if (fault == 7) wrongManifest.kind = ContentKind::Epub;
    if (fault == 8) wrongManifest.formatVersion = 2;
    if (fault == 9) ++wrongManifest.length;
    DictionaryExtractionJournal journal(storage, scratch);
    DictionaryExtractionParent authorized(journal, wrongParent, wrongManifest, wrongGeneration);
    EXPECT_EQ(authorized.begin(initial), DictionaryJournalResult::Conflict) << fault;
    EXPECT_TRUE(storage.files.empty());
    EXPECT_EQ(authorized.current(), nullptr);
  }
  DictionaryExtractionJournal journal(storage, scratch);
  DictionaryExtractionParent authorized(journal, parent, manifest, initial.generation);
  ASSERT_EQ(authorized.begin(initial), DictionaryJournalResult::Ok);
  parent.phase = TransferPhase::Committed;
  EXPECT_EQ(authorized.recover(initial), DictionaryJournalResult::Conflict);
  EXPECT_EQ(authorized.current(), nullptr);
  parent.phase = TransferPhase::Installing;
  ASSERT_EQ(authorized.recover(initial), DictionaryJournalResult::Ok);
  for (unsigned member : {2u, 1u, 0u, 3u})
    ASSERT_EQ(authorized.recordSealed(member, hash), DictionaryJournalResult::Ok);
  parent.phase = TransferPhase::Committed;
  ASSERT_EQ(authorized.recover(initial), DictionaryJournalResult::Ok);
  EXPECT_EQ(authorized.current()->sealed, 15u);
  const auto writes = storage.writes;
  EXPECT_EQ(authorized.recordSealed(2, hash), DictionaryJournalResult::Conflict);
  EXPECT_EQ(storage.writes, writes);
}
TEST_F(DictionaryExtractionJournalTest, DurableParentDiscoveryChecksBothSlotsAndPreservesFiles) {
  DictionaryExtractionJournal journal(storage, scratch);
  ASSERT_EQ(journal.begin(initial), DictionaryJournalResult::Ok);
  ASSERT_EQ(journal.recordSealed(2, hash), DictionaryJournalResult::Ok);
  const auto saved = storage.files;
  DictionaryExtractionJournal reopened(storage, scratch);
  ASSERT_EQ(reopened.recover(initial.transaction, initial.generation, initial.archiveHash),
            DictionaryJournalResult::Ok);
  EXPECT_EQ(reopened.current()->sealed, 4u);
  EXPECT_EQ(storage.files, saved);
  for (unsigned fault = 0; fault < 6; ++fault) {
    storage.files = saved;
    auto foreign = initial;
    if (fault == 0) foreign.transaction[0] ^= 0x80;
    if (fault == 1) foreign.generation[0] ^= 1;
    if (fault == 2) foreign.archiveHash[0] ^= 1;
    if (fault == 3) ++foreign.lengths[0];
    if (fault == 4) foreign.compressed = !foreign.compressed;
    if (fault == 5) {
      foreign.synonyms = false;
      foreign.lengths[3] = 0;
    }
    put(0, foreign);
    const auto changed = storage.files;
    EXPECT_EQ(reopened.recover(initial.transaction, initial.generation, initial.archiveHash),
              DictionaryJournalResult::Conflict)
        << fault;
    EXPECT_EQ(reopened.current(), nullptr);
    EXPECT_EQ(storage.files, changed);
  }
  storage.files = saved;
  storage.files[DICTIONARY_EXTRACTION_JOURNALS[0]][0] ^= 1;
  ASSERT_EQ(reopened.recover(initial.transaction, initial.generation, initial.archiveHash),
            DictionaryJournalResult::Ok);
  EXPECT_EQ(reopened.current()->sealed, 4u);
  storage.failRead = true;
  EXPECT_EQ(reopened.recover(initial.transaction, initial.generation, initial.archiveHash),
            DictionaryJournalResult::IoError);
  EXPECT_EQ(reopened.current(), nullptr);
  storage.failRead = false;
  storage.files.clear();
  EXPECT_EQ(reopened.recover(initial.transaction, initial.generation, initial.archiveHash),
            DictionaryJournalResult::Missing);
  EXPECT_EQ(reopened.recover({}, initial.generation, initial.archiveHash), DictionaryJournalResult::Invalid);
}

TEST_F(DictionaryExtractionJournalTest, ParentRehydratesReceiptDuringInstallingAndCommittedRecovery) {
  DictionaryExtractionJournal journal(storage, scratch);
  ASSERT_EQ(journal.begin(initial), DictionaryJournalResult::Ok);
  for (unsigned member : {2u, 1u, 0u, 3u}) ASSERT_EQ(journal.recordSealed(member, hash), DictionaryJournalResult::Ok);
  TransferState state;
  state.owner[0] = 9;
  state.transaction = initial.transaction;
  state.storageGeneration = initial.generation;
  state.contentHash = initial.archiveHash;
  state.length = state.durableOffset = 1000;
  ContentManifest manifest;
  manifest.kind = ContentKind::Dictionary;
  manifest.formatVersion = 1;
  manifest.length = state.length;
  manifest.contentHash = state.contentHash;
  const auto saved = storage.files;
  for (auto phase : {TransferPhase::Installing, TransferPhase::Committed}) {
    state.phase = phase;
    DictionaryExtractionJournal reopened(storage, scratch);
    DictionaryExtractionParent parent(reopened, state, manifest, initial.generation);
    ASSERT_EQ(parent.recover(), DictionaryJournalResult::Ok);
    ASSERT_NE(parent.current(), nullptr);
    EXPECT_EQ(parent.current()->sealed, 15u);
    --state.durableOffset;
    EXPECT_EQ(parent.recover(), DictionaryJournalResult::Conflict);
    EXPECT_EQ(parent.current(), nullptr);
    ++state.durableOffset;
    ASSERT_EQ(parent.recover(), DictionaryJournalResult::Ok);
    state.owner.fill(0);
    EXPECT_EQ(parent.recover(), DictionaryJournalResult::Conflict);
    EXPECT_EQ(parent.current(), nullptr);
    state.owner[0] = 9;
    EXPECT_EQ(storage.files, saved);
  }
}
}  // namespace
