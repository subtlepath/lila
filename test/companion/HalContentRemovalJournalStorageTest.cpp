#include <gtest/gtest.h>

#include "lib/hal/HalContentRemovalJournalStorage.h"
using namespace companion;
namespace {
ContentRemovalRecord initial() {
  ContentRemovalRecord record;
  record.request.transaction.fill(1);
  record.request.owner.fill(2);
  record.request.generation.fill(3);
  record.request.manifest.contentHash.fill(4);
  record.request.manifest.kind = ContentKind::Dictionary;
  record.request.manifest.length = 1234;
  record.request.manifest.formatVersion = 1;
  record.planHash.fill(5);
  return record;
}
class HalRemovalJournalTest : public testing::Test {
 protected:
  std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> scratch{};
  HalContentRemovalJournalStorage storage;
  ContentRemovalJournal journal{storage, scratch};
  void SetUp() override {
    inventory_hal_test::state = {};
    inventory_hal_test::state.enumerateFileMap = true;
  }
};
}  // namespace
TEST_F(HalRemovalJournalTest, DurableRecordSurvivesReconstructionAndReusesHandles) {
  ASSERT_EQ(journal.begin(initial()), ContentRemovalJournalResult::Ok);
  ASSERT_EQ(journal.advance(ContentRemovalPhase::Quarantined), ContentRemovalJournalResult::Ok);
  const auto wrappers = inventory_hal_test::state.preparations;
  inventory_hal_test::state.falseExists = true;
  for (unsigned repeat = 0; repeat < 5; ++repeat) {
    ASSERT_EQ(journal.recover(initial()), ContentRemovalJournalResult::Ok);
    ASSERT_EQ(journal.confirmRecovered(), ContentRemovalJournalResult::Ok);
    EXPECT_EQ(journal.current()->phase, ContentRemovalPhase::Quarantined);
    EXPECT_EQ(inventory_hal_test::state.preparations, wrappers);
  }
  HalContentRemovalJournalStorage rebooted;
  ContentRemovalJournal recovered(rebooted, scratch);
  ASSERT_EQ(recovered.recover(initial()), ContentRemovalJournalResult::Ok);
  EXPECT_EQ(recovered.current()->phase, ContentRemovalPhase::Quarantined);
}
TEST_F(HalRemovalJournalTest, RejectsUnownedPathsAndIncorrectRecordLengthsBeforeWrites) {
  ASSERT_TRUE(storage.prepare());
  const auto files = inventory_hal_test::state.files;
  for (const auto* path : {"/.crosspoint/companion/other", "../removal-a", "/.crosspoint/companion/REMOVAL-A"}) {
    uint64_t size = 77;
    EXPECT_EQ(storage.stat(path, size), FileStatus::Error);
    EXPECT_EQ(size, 77);
    EXPECT_FALSE(storage.read(path, scratch));
    EXPECT_FALSE(storage.write(path, scratch));
  }
  EXPECT_FALSE(storage.write(CONTENT_REMOVAL_JOURNALS[0], std::span(scratch).first(scratch.size() - 1)));
  EXPECT_EQ(inventory_hal_test::state.files, files);
}
TEST_F(HalRemovalJournalTest, DirectoryCollisionAndEnumerationFailurePreserveEvidence) {
  ASSERT_TRUE(storage.prepare());
  auto& state = inventory_hal_test::state;
  state.directories[CONTENT_REMOVAL_JOURNALS[0]] = {};
  uint64_t size = 99;
  EXPECT_EQ(storage.stat(CONTENT_REMOVAL_JOURNALS[0], size), FileStatus::Error);
  EXPECT_FALSE(storage.write(CONTENT_REMOVAL_JOURNALS[0], scratch));
  EXPECT_EQ(size, 99);
  EXPECT_TRUE(state.files.empty());
  state.directories.erase(CONTENT_REMOVAL_JOURNALS[0]);
  state.directoryErrorPath = TRANSFER_DIRECTORY;
  EXPECT_EQ(storage.stat(CONTENT_REMOVAL_JOURNALS[0], size), FileStatus::Error);
  EXPECT_FALSE(storage.write(CONTENT_REMOVAL_JOURNALS[0], scratch));
  EXPECT_TRUE(state.files.empty());
}
TEST_F(HalRemovalJournalTest, FailedWriteTruncateSyncAndCloseInvalidateUntilRecovery) {
  for (unsigned mode = 0; mode < 4; ++mode) {
    inventory_hal_test::state = {};
    auto& state = inventory_hal_test::state;
    state.enumerateFileMap = true;
    HalContentRemovalJournalStorage concrete;
    ContentRemovalJournal local(concrete, scratch);
    ASSERT_EQ(local.begin(initial()), ContentRemovalJournalResult::Ok);
    state.failWrite = mode == 0;
    state.failTruncate = mode == 1;
    state.failSync = mode == 2;
    state.failClose = mode == 3;
    EXPECT_EQ(local.advance(ContentRemovalPhase::Quarantined), ContentRemovalJournalResult::IoError);
    EXPECT_EQ(local.current(), nullptr);
    state.failWrite = state.failTruncate = state.failSync = state.failClose = false;
    ASSERT_EQ(local.recover(initial()), ContentRemovalJournalResult::Ok);
    ASSERT_EQ(local.confirmRecovered(), ContentRemovalJournalResult::Ok);
    EXPECT_EQ(local.advance(ContentRemovalPhase::Quarantined), ContentRemovalJournalResult::Ok);
  }
}
TEST_F(HalRemovalJournalTest, ShortReadsAndSilentWriteCorruptionCannotAdvanceLiveState) {
  ASSERT_EQ(journal.begin(initial()), ContentRemovalJournalResult::Ok);
  auto& state = inventory_hal_test::state;
  state.corruptWrite = true;
  EXPECT_EQ(journal.advance(ContentRemovalPhase::Quarantined), ContentRemovalJournalResult::Corrupt);
  EXPECT_EQ(journal.current(), nullptr);
  state.corruptWrite = false;
  ASSERT_EQ(journal.recover(initial()), ContentRemovalJournalResult::Ok);
  EXPECT_EQ(journal.current()->phase, ContentRemovalPhase::Prepared);
  state.shortRead = state.reads + 1;
  EXPECT_EQ(journal.recover(initial()), ContentRemovalJournalResult::IoError);
  EXPECT_EQ(journal.current(), nullptr);
}
