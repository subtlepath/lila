#include <gtest/gtest.h>

#include "lib/hal/HalContentRemovalJournalStorage.h"
#include "lib/hal/HalRemovalMetadataAuthorization.h"
#include "lib/hal/HalRemovalMetadataSnapshotStorage.h"
using namespace companion;
namespace {
RemovalMetadataSnapshot snapshot() {
  RemovalMetadataSnapshot result;
  result.request.transaction.fill(1);
  result.request.owner.fill(2);
  result.request.generation.fill(3);
  result.request.manifest.kind = ContentKind::Epub;
  result.request.manifest.formatVersion = 1;
  result.request.manifest.contentHash.fill(4);
  result.request.manifest.length = 10;
  result.planHash.fill(5);
  result.previousHash.fill(6);
  result.nextHash.fill(7);
  result.previousLength = 10;
  result.nextLength = 20;
  return result;
}
void reset() { inventory_hal_test::state = {}; }
std::string target() {
  std::string result = "/.crosspoint/companion/removal-snapshot-";
  for (unsigned at = 0; at < 32; ++at) result += "05";
  return result + "s";
}
}  // namespace
TEST(RemovalMetadataStorage, RequiresExactQuarantinedOwnerAndRetainsJournal) {
  reset();
  std::array<uint8_t, 168> journalBytes{};
  std::array<uint8_t, 240> bytes{};
  HalContentRemovalJournalStorage journalStorage;
  ContentRemovalJournal journal(journalStorage, journalBytes);
  HalRemovalMetadataSnapshotStorage storage(bytes);
  const auto expected = snapshot();
  ContentRemovalRecord record{expected.request, expected.planHash};
  EXPECT_EQ(storage.persist(expected, journal), RemovalMetadataStorageResult::Invalid);
  ASSERT_EQ(journal.begin(record), ContentRemovalJournalResult::Ok);
  EXPECT_EQ(storage.persist(expected, journal), RemovalMetadataStorageResult::Invalid);
  ASSERT_EQ(journal.advance(ContentRemovalPhase::Quarantined), ContentRemovalJournalResult::Ok);
  auto foreign = expected;
  foreign.request.owner.fill(8);
  EXPECT_EQ(storage.persist(foreign, journal), RemovalMetadataStorageResult::Invalid);
  ASSERT_EQ(storage.persist(expected, journal), RemovalMetadataStorageResult::Ok);
  EXPECT_EQ(journal.current()->phase, ContentRemovalPhase::Quarantined);
  RemovalMetadataSnapshot output;
  EXPECT_EQ(storage.load(expected.planHash, expected.file, output), RemovalMetadataStorageResult::Ok);
  EXPECT_EQ(output, expected);
  const auto renames = inventory_hal_test::state.renames;
  EXPECT_EQ(storage.persist(expected, journal), RemovalMetadataStorageResult::Ok);
  EXPECT_EQ(inventory_hal_test::state.renames, renames);
}
TEST(RemovalMetadataStorage, RecreatedOwnerRecoversWriteSyncCloseAndRenameFailures) {
  for (unsigned fault = 0; fault < 6; ++fault) {
    reset();
    auto& state = inventory_hal_test::state;
    std::array<uint8_t, 168> journalBytes{};
    std::array<uint8_t, 240> bytes{};
    HalContentRemovalJournalStorage journalStorage;
    ContentRemovalJournal journal(journalStorage, journalBytes);
    const auto expected = snapshot();
    ContentRemovalRecord record{expected.request, expected.planHash};
    ASSERT_EQ(journal.begin(record), ContentRemovalJournalResult::Ok);
    ASSERT_EQ(journal.advance(ContentRemovalPhase::Quarantined), ContentRemovalJournalResult::Ok);
    if (fault == 0) state.failWrite = true;
    if (fault == 1) state.failSync = true;
    if (fault == 2) state.failClose = true;
    if (fault == 3) state.failRename = 1;
    if (fault == 4) state.failRenameAfter = 1;
    if (fault == 5) state.corruptWrite = true;
    {
      HalRemovalMetadataSnapshotStorage storage(bytes);
      EXPECT_NE(storage.persist(expected, journal), RemovalMetadataStorageResult::Ok);
    }
    state.failWrite = state.failSync = state.failClose = state.corruptWrite = false;
    state.failRename = state.failRenameAfter = 0;
    ContentRemovalJournal recovered(journalStorage, journalBytes);
    ASSERT_EQ(recovered.recover(record), ContentRemovalJournalResult::Ok);
    ASSERT_EQ(recovered.confirmRecovered(), ContentRemovalJournalResult::Ok);
    HalRemovalMetadataSnapshotStorage storage(bytes);
    ASSERT_EQ(storage.persist(expected, recovered), RemovalMetadataStorageResult::Ok);
    RemovalMetadataSnapshot output;
    ASSERT_EQ(storage.load(expected.planHash, expected.file, output), RemovalMetadataStorageResult::Ok);
    EXPECT_EQ(output, expected);
  }
}
TEST(RemovalMetadataStorage, ForeignAndDamagedPublishedDeclarationsArePreserved) {
  reset();
  auto& state = inventory_hal_test::state;
  std::array<uint8_t, 168> journalBytes{};
  std::array<uint8_t, 240> bytes{};
  HalContentRemovalJournalStorage journalStorage;
  ContentRemovalJournal journal(journalStorage, journalBytes);
  const auto expected = snapshot();
  ContentRemovalRecord record{expected.request, expected.planHash};
  ASSERT_EQ(journal.begin(record), ContentRemovalJournalResult::Ok);
  ASSERT_EQ(journal.advance(ContentRemovalPhase::Quarantined), ContentRemovalJournalResult::Ok);
  HalRemovalMetadataSnapshotStorage storage(bytes);
  auto foreign = expected;
  foreign.request.owner.fill(8);
  ASSERT_EQ(encodeRemovalMetadataSnapshot(foreign, bytes), bytes.size());
  state.files[target()] = {bytes.begin(), bytes.end()};
  const auto foreignBytes = state.files[target()];
  EXPECT_EQ(storage.persist(expected, journal), RemovalMetadataStorageResult::Conflict);
  EXPECT_EQ(state.files[target()], foreignBytes);
  state.files[target()][0] ^= 1;
  const auto damaged = state.files[target()];
  EXPECT_EQ(storage.persist(expected, journal), RemovalMetadataStorageResult::Corrupt);
  auto output = foreign;
  EXPECT_EQ(storage.load(expected.planHash, expected.file, output), RemovalMetadataStorageResult::Corrupt);
  EXPECT_EQ(output, foreign);
  EXPECT_EQ(state.files[target()], damaged);
}
TEST(RemovalMetadataStorage, ForeignUnpublishedStagesAndDirectoryCollisionsArePreserved) {
  for (unsigned fault = 0; fault < 3; ++fault) {
    reset();
    auto& state = inventory_hal_test::state;
    std::array<uint8_t, 168> journalBytes{};
    std::array<uint8_t, 240> bytes{};
    HalContentRemovalJournalStorage journalStorage;
    ContentRemovalJournal journal(journalStorage, journalBytes);
    const auto expected = snapshot();
    ContentRemovalRecord record{expected.request, expected.planHash};
    ASSERT_EQ(journal.begin(record), ContentRemovalJournalResult::Ok);
    ASSERT_EQ(journal.advance(ContentRemovalPhase::Quarantined), ContentRemovalJournalResult::Ok);
    if (fault == 0) {
      auto foreign = expected;
      foreign.nextHash.fill(9);
      ASSERT_EQ(encodeRemovalMetadataSnapshot(foreign, bytes), bytes.size());
      state.files[target() + ".tmp"] = {bytes.begin(), bytes.end()};
    } else if (fault == 1) {
      state.directories[target() + ".tmp"] = {};
    } else {
      state.files[target() + ".tmp"] = std::vector<uint8_t>(241, 1);
    }
    const auto before = state.files;
    HalRemovalMetadataSnapshotStorage storage(bytes);
    EXPECT_NE(storage.persist(expected, journal), RemovalMetadataStorageResult::Ok);
    EXPECT_EQ(state.files, before);
  }
}

TEST(RemovalMetadataStorage, AuthorizationRequiresDurableProofAndInvalidatesOnPhaseChange) {
  reset();
  std::array<uint8_t, 168> journalBytes{};
  std::array<uint8_t, 240> bytes{};
  HalContentRemovalJournalStorage journalStorage;
  ContentRemovalJournal journal(journalStorage, journalBytes);
  HalRemovalMetadataSnapshotStorage storage(bytes);
  HalRemovalMetadataAuthorization authorization(journal, storage);
  const auto expected = snapshot();
  ContentRemovalRecord record{expected.request, expected.planHash};
  ASSERT_EQ(journal.begin(record), ContentRemovalJournalResult::Ok);
  EXPECT_FALSE(authorization.bind(expected));
  ASSERT_EQ(journal.advance(ContentRemovalPhase::Quarantined), ContentRemovalJournalResult::Ok);
  EXPECT_FALSE(authorization.bind(expected));
  ASSERT_EQ(storage.persist(expected, journal), RemovalMetadataStorageResult::Ok);
  ASSERT_TRUE(authorization.bind(expected));
  EXPECT_TRUE(HalRemovalMetadataAuthorization::check(&authorization, expected));
  auto foreign = expected;
  foreign.request.owner.fill(9);
  EXPECT_FALSE(HalRemovalMetadataAuthorization::check(&authorization, foreign));
  ASSERT_EQ(journal.advance(ContentRemovalPhase::Committed), ContentRemovalJournalResult::Ok);
  EXPECT_FALSE(HalRemovalMetadataAuthorization::check(&authorization, expected));
  ASSERT_TRUE(authorization.bind(expected));
  EXPECT_TRUE(HalRemovalMetadataAuthorization::check(&authorization, expected));
  ASSERT_EQ(journal.advance(ContentRemovalPhase::Retired), ContentRemovalJournalResult::Ok);
  EXPECT_FALSE(HalRemovalMetadataAuthorization::check(&authorization, expected));
  ASSERT_TRUE(authorization.bind(expected));
  EXPECT_TRUE(HalRemovalMetadataAuthorization::check(&authorization, expected));
  inventory_hal_test::state.files[target()][0] ^= 1;
  EXPECT_FALSE(authorization.bind(expected));
  EXPECT_FALSE(HalRemovalMetadataAuthorization::check(&authorization, expected));
}
