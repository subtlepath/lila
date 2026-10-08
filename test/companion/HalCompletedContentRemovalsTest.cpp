#include <gtest/gtest.h>

#include "lib/hal/HalCompletedContentRemovals.h"
#include "lib/hal/HalCompletedRemovalJournalRelease.h"
#include "lib/hal/HalContentRemovalJournalStorage.h"
using namespace companion;
namespace {
ContentRemovalRecord initial() {
  ContentRemovalRecord result;
  result.request.transaction.fill(1);
  result.request.owner.fill(2);
  result.request.generation.fill(3);
  result.request.manifest.kind = ContentKind::Epub;
  result.request.manifest.formatVersion = 1;
  result.request.manifest.length = 123;
  result.request.manifest.contentHash.fill(4);
  result.planHash.fill(5);
  return result;
}
std::string path() {
  std::string result = "/.crosspoint/companion/removal-done-";
  for (unsigned at = 0; at < 16; ++at) result += "01";
  return result;
}
void retire(ContentRemovalJournal& journal, const ContentRemovalRecord& record) {
  ASSERT_EQ(journal.begin(record), ContentRemovalJournalResult::Ok);
  ASSERT_EQ(journal.advance(ContentRemovalPhase::Quarantined), ContentRemovalJournalResult::Ok);
  ASSERT_EQ(journal.advance(ContentRemovalPhase::Committed), ContentRemovalJournalResult::Ok);
  ASSERT_EQ(journal.confirmRecovered(), ContentRemovalJournalResult::Ok);
  ASSERT_EQ(journal.advance(ContentRemovalPhase::Retired), ContentRemovalJournalResult::Ok);
}
}  // namespace
TEST(CompletedContentRemovals, RequiresRetiredOwnerAndLoadsWithoutCurrentMetadata) {
  inventory_hal_test::state = {};
  std::array<uint8_t, 168> journalScratch{}, receiptScratch{};
  HalContentRemovalJournalStorage storage;
  ContentRemovalJournal journal(storage, journalScratch);
  HalCompletedContentRemovals completions(receiptScratch);
  const auto record = initial();
  EXPECT_EQ(completions.persist(record, journal), CompletedRemovalResult::Invalid);
  retire(journal, record);
  const auto retired = *journal.current();
  ASSERT_EQ(completions.persist(retired, journal), CompletedRemovalResult::Ok);
  const auto renames = inventory_hal_test::state.renames;
  EXPECT_EQ(completions.persist(retired, journal), CompletedRemovalResult::Ok);
  EXPECT_EQ(inventory_hal_test::state.renames, renames);
  const std::string changedState = R"({"openEpubPath":"/other.epub"})";
  inventory_hal_test::state.files["/.crosspoint/state.json"] = {changedState.begin(), changedState.end()};
  ContentRemovalRecord output;
  EXPECT_EQ(completions.load(record.request, output), CompletedRemovalResult::Ok);
  EXPECT_EQ(output, retired);
  for (unsigned field = 0; field < 4; ++field) {
    auto foreign = record.request;
    if (field == 0) foreign.owner.fill(8);
    if (field == 1) foreign.generation.fill(8);
    if (field == 2) foreign.manifest.contentHash.fill(8);
    if (field == 3) ++foreign.manifest.length;
    EXPECT_EQ(completions.load(foreign, output), CompletedRemovalResult::Conflict);
    EXPECT_EQ(output, retired);
  }
}
TEST(CompletedContentRemovals, ReconstructionRecoversAmbiguousPersistence) {
  for (unsigned fault = 0; fault < 6; ++fault) {
    auto& state = inventory_hal_test::state;
    state = {};
    std::array<uint8_t, 168> journalScratch{}, receiptScratch{};
    HalContentRemovalJournalStorage storage;
    ContentRemovalJournal journal(storage, journalScratch);
    const auto record = initial();
    retire(journal, record);
    const auto retired = *journal.current();
    if (fault == 0) state.failWrite = true;
    if (fault == 1) state.failSync = true;
    if (fault == 2) state.failClose = true;
    if (fault == 3) state.failRename = 1;
    if (fault == 4) state.failRenameAfter = 1;
    if (fault == 5) state.corruptWrite = true;
    {
      HalCompletedContentRemovals completions(receiptScratch);
      EXPECT_NE(completions.persist(retired, journal), CompletedRemovalResult::Ok);
    }
    state.failWrite = state.failSync = state.failClose = state.corruptWrite = false;
    state.failRename = state.failRenameAfter = 0;
    ContentRemovalJournal recovered(storage, journalScratch);
    ASSERT_EQ(recovered.recover(record), ContentRemovalJournalResult::Ok);
    HalCompletedContentRemovals completions(receiptScratch);
    EXPECT_EQ(completions.persist(*recovered.current(), recovered), CompletedRemovalResult::Ok);
    ContentRemovalRecord output;
    ASSERT_EQ(completions.load(record.request, output), CompletedRemovalResult::Ok);
    EXPECT_EQ(output, retired);
  }
}
TEST(CompletedContentRemovals, ConflictingAndCorruptPublishedReceiptsRemainUntouched) {
  auto& state = inventory_hal_test::state;
  state = {};
  std::array<uint8_t, 168> journalScratch{}, receiptScratch{};
  HalContentRemovalJournalStorage storage;
  ContentRemovalJournal journal(storage, journalScratch);
  const auto record = initial();
  retire(journal, record);
  auto foreign = *journal.current();
  foreign.planHash.fill(8);
  ASSERT_EQ(encodeContentRemovalRecord(foreign, receiptScratch), receiptScratch.size());
  state.files[path()] = {receiptScratch.begin(), receiptScratch.end()};
  const auto before = state.files;
  HalCompletedContentRemovals completions(receiptScratch);
  EXPECT_EQ(completions.persist(*journal.current(), journal), CompletedRemovalResult::Conflict);
  EXPECT_EQ(state.files, before);
  state.files[path()][0] ^= 1;
  const auto damaged = state.files;
  EXPECT_EQ(completions.persist(*journal.current(), journal), CompletedRemovalResult::Corrupt);
  EXPECT_EQ(state.files, damaged);
  ContentRemovalRecord output;
  EXPECT_EQ(completions.load(record.request, output), CompletedRemovalResult::Corrupt);
}

TEST(CompletedContentRemovals, InterruptedReleaseUsesReceiptToReleaseSurvivingOlderSlot) {
  for (unsigned fault = 0; fault < 3; ++fault) {
    auto& state = inventory_hal_test::state;
    state = {};
    std::array<uint8_t, 168> journalScratch{}, receiptScratch{}, releaseScratch{};
    HalContentRemovalJournalStorage storage;
    ContentRemovalJournal journal(storage, journalScratch);
    const auto record = initial();
    retire(journal, record);
    HalCompletedContentRemovals completions(receiptScratch);
    ASSERT_EQ(completions.persist(*journal.current(), journal), CompletedRemovalResult::Ok);
    if (fault == 1) state.failRemove = true;
    if (fault == 2) state.failRemoveAfter = true;
    HalCompletedRemovalJournalRelease release(journal, storage, completions, releaseScratch);
    const auto result = release.release();
    EXPECT_EQ(result == CompletedRemovalResult::Ok, fault == 0);
    EXPECT_FALSE(journal.current());
    state.failRemove = state.failRemoveAfter = false;
    ContentRemovalJournal recovered(storage, journalScratch);
    const auto discovered = recovered.recover(record.request.generation);
    if (discovered == ContentRemovalJournalResult::Ok) {
      if (fault == 2) EXPECT_EQ(recovered.current()->phase, ContentRemovalPhase::Committed);
      HalCompletedRemovalJournalRelease retry(recovered, storage, completions, releaseScratch);
      EXPECT_EQ(retry.release(), CompletedRemovalResult::Ok);
    } else {
      EXPECT_EQ(discovered, ContentRemovalJournalResult::Missing);
    }
    auto next = record;
    next.request.transaction.fill(9);
    next.planHash.fill(9);
    EXPECT_EQ(recovered.begin(next), ContentRemovalJournalResult::Ok);
    ContentRemovalRecord completed;
    EXPECT_EQ(completions.load(record.request, completed), CompletedRemovalResult::Ok);
    EXPECT_EQ(completed.phase, ContentRemovalPhase::Retired);
  }
}
TEST(CompletedContentRemovals, ReleaseChecksBothOwnersBeforeDeletingEitherSlot) {
  auto& state = inventory_hal_test::state;
  state = {};
  std::array<uint8_t, 168> journalScratch{}, receiptScratch{}, releaseScratch{};
  HalContentRemovalJournalStorage storage;
  ContentRemovalJournal journal(storage, journalScratch);
  const auto record = initial();
  retire(journal, record);
  HalCompletedContentRemovals completions(receiptScratch);
  ASSERT_EQ(completions.persist(*journal.current(), journal), CompletedRemovalResult::Ok);
  auto foreign = *journal.current();
  foreign.request.owner.fill(9);
  ASSERT_EQ(encodeContentRemovalRecord(foreign, releaseScratch), releaseScratch.size());
  state.files[CONTENT_REMOVAL_JOURNALS[1]] = {releaseScratch.begin(), releaseScratch.end()};
  const auto before = state.files;
  HalCompletedRemovalJournalRelease release(journal, storage, completions, releaseScratch);
  EXPECT_EQ(release.release(), CompletedRemovalResult::Conflict);
  EXPECT_EQ(state.files, before);
}
