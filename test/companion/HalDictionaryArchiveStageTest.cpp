#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <iterator>
#include <vector>

#include "lib/Companion/CompanionDictionaryMembersValidation.h"
#include "lib/hal/HalDictionaryArchiveStage.h"
#include "lib/hal/HalDictionaryBundleSource.h"
#include "lib/hal/HalTintaDerivedCandidateStage.h"

using namespace companion;
namespace {
class HalDictionaryArchiveStageTest : public testing::Test {
 protected:
  std::array<uint8_t, 64> scratch;
  HalDictionaryArchiveStage stage{scratch};
  static constexpr std::array<uint8_t, 32> BYTES = {'P', 'K'};
  void SetUp() override { inventory_hal_test::state = {}; }
  static std::vector<uint8_t> fixture(const char* name) {
    std::ifstream file(std::string(COMPANION_FIXTURE_DIR) + "/" + name, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
  }
};
TEST_F(HalDictionaryArchiveStageTest, TintaCandidateRequiresManifestHashAndNeverChangesActiveFiles) {
  const auto bytes = fixture("TintaDerivedManifest-v1.fixture");
  TintaDerivedManifestView manifest;
  ASSERT_TRUE(manifest.decode(bytes));
  Identity course{}, storage{};
  Digest pack{}, frontier{};
  course.fill(7);
  storage.fill(1);
  pack.fill(3);
  frontier.fill(4);
  std::array<char, COURSE_STATE_PATH_SIZE> active{}, candidate{};
  ASSERT_TRUE(tintaDerivedFilePath(course, TintaDerivedFile::Days, TintaDerivedRole::Active, active));
  ASSERT_TRUE(tintaDerivedFilePath(course, TintaDerivedFile::Days, TintaDerivedRole::Candidate, candidate));
  auto& files = inventory_hal_test::state.files;
  files[active.data()] = {42};
  std::array<char, COURSE_STATE_DIRECTORY_SIZE> root{};
  ASSERT_TRUE(courseStateDirectory(course, root));
  inventory_hal_test::state.directories[root.data()] = {};
  inventory_hal_test::state.enumerateFileMap = true;
  HalTintaDerivedCandidateStage staged(scratch);
  ASSERT_TRUE(staged.begin(manifest, TintaDerivedFile::Days, course, storage, pack, frontier));
  const std::array<uint8_t, 4> wrong{'B', 'A', 'D', '!'};
  ASSERT_TRUE(staged.write(0, wrong));
  EXPECT_FALSE(staged.seal());
  EXPECT_FALSE(staged.isSealed());
  staged.abort();
  EXPECT_FALSE(files.contains(candidate.data()));
  EXPECT_EQ(files[active.data()], (std::vector<uint8_t>{42}));
  ASSERT_TRUE(staged.begin(manifest, TintaDerivedFile::Days, course, storage, pack, frontier));
  const std::array<uint8_t, 4> correct{'T', 'D', 'L', '1'};
  ASSERT_TRUE(staged.write(0, correct));
  ASSERT_TRUE(staged.seal());
  EXPECT_TRUE(staged.isSealed());
  staged.abort();
  EXPECT_EQ(files[candidate.data()], (std::vector<uint8_t>(correct.begin(), correct.end())));
  EXPECT_EQ(files[active.data()], (std::vector<uint8_t>{42}));
  EXPECT_FALSE(staged.begin(manifest, TintaDerivedFile::Days, course, storage, pack, frontier));
  EXPECT_FALSE(staged.isSealed());
  frontier.fill(9);
  EXPECT_FALSE(staged.begin(manifest, TintaDerivedFile::Items, course, storage, pack, frontier));
  EXPECT_EQ(files[active.data()], (std::vector<uint8_t>{42}));
}
TEST_F(HalDictionaryArchiveStageTest, TintaCleanupFailureCannotRedirectOwnedCandidateRemoval) {
  const auto bytes = fixture("TintaDerivedManifest-v1.fixture");
  TintaDerivedManifestView manifest;
  ASSERT_TRUE(manifest.decode(bytes));
  Identity course{}, storage{};
  Digest pack{}, frontier{};
  course.fill(7);
  storage.fill(1);
  pack.fill(3);
  frontier.fill(4);
  std::array<char, COURSE_STATE_DIRECTORY_SIZE> root{};
  std::array<char, COURSE_STATE_PATH_SIZE> days{}, reviews{};
  ASSERT_TRUE(courseStateDirectory(course, root));
  ASSERT_TRUE(tintaDerivedFilePath(course, TintaDerivedFile::Days, TintaDerivedRole::Candidate, days));
  ASSERT_TRUE(tintaDerivedFilePath(course, TintaDerivedFile::LocalReviews, TintaDerivedRole::Candidate, reviews));
  auto& state = inventory_hal_test::state;
  state.directories[root.data()] = {};
  state.enumerateFileMap = true;
  state.files[reviews.data()] = {99};
  HalTintaDerivedCandidateStage staged(scratch);
  ASSERT_TRUE(staged.begin(manifest, TintaDerivedFile::Days, course, storage, pack, frontier));
  const std::array<uint8_t, 1> partial{1};
  ASSERT_TRUE(staged.write(0, partial));
  state.failRemove = true;
  EXPECT_FALSE(staged.begin(manifest, TintaDerivedFile::LocalReviews, course, storage, pack, frontier));
  EXPECT_EQ(state.files[days.data()], (std::vector<uint8_t>{1}));
  EXPECT_EQ(state.files[reviews.data()], (std::vector<uint8_t>{99}));
  state.failRemove = false;
  staged.abort();
  EXPECT_FALSE(state.files.contains(days.data()));
  EXPECT_EQ(state.files[reviews.data()], (std::vector<uint8_t>{99}));
  EXPECT_FALSE(staged.isSealed());
  EXPECT_FALSE(staged.begin(manifest, TintaDerivedFile::LocalReviews, course, storage, pack, frontier));
  state.files.erase(reviews.data());
  ASSERT_TRUE(staged.begin(manifest, TintaDerivedFile::LocalReviews, course, storage, pack, frontier));
  ASSERT_TRUE(staged.seal());
  EXPECT_TRUE(staged.isSealed());
  EXPECT_TRUE(state.files[reviews.data()].empty());
  frontier.fill(9);
  EXPECT_FALSE(staged.begin(manifest, TintaDerivedFile::Days, course, storage, pack, frontier));
  EXPECT_FALSE(staged.isSealed());
  EXPECT_TRUE(state.files[reviews.data()].empty());
}
TEST_F(HalDictionaryArchiveStageTest, TintaResumeRehashesDurablePrefixTrimsTailAndRequiresFinalManifestHash) {
  const auto bytes = fixture("TintaDerivedManifest-v1.fixture");
  TintaDerivedManifestView manifest;
  ASSERT_TRUE(manifest.decode(bytes));
  TintaReceiveCheckpoint checkpoint;
  checkpoint.course.fill(7);
  checkpoint.storage.fill(1);
  checkpoint.transaction.fill(8);
  checkpoint.owner.fill(9);
  checkpoint.file = TintaDerivedFile::Days;
  checkpoint.sealedMask = 15;
  checkpoint.offset = 2;
  checkpoint.length = 4;
  checkpoint.sequence = 1;
  Digest pack{}, frontier{};
  pack.fill(3);
  frontier.fill(4);
  ASSERT_NE(SHA256(bytes.data(), bytes.size(), checkpoint.manifestHash.data()), nullptr);
  std::array<char, COURSE_STATE_DIRECTORY_SIZE> root{};
  std::array<char, COURSE_STATE_PATH_SIZE> path{};
  ASSERT_TRUE(courseStateDirectory(checkpoint.course, root));
  ASSERT_TRUE(tintaDerivedFilePath(checkpoint.course, checkpoint.file, TintaDerivedRole::Candidate, path));
  auto& state = inventory_hal_test::state;
  state.directories[root.data()] = {};
  state.enumerateFileMap = true;
  for (bool corrupt : {false, true}) {
    state.files[path.data()] = {corrupt ? uint8_t('X') : uint8_t('T'), 'D', 99, 99, 99};
    HalTintaDerivedCandidateStage resumed(scratch);
    ASSERT_TRUE(resumed.resume(checkpoint, manifest, checkpoint.manifestHash, checkpoint.transaction, checkpoint.owner,
                               checkpoint.course, checkpoint.storage, pack, frontier));
    EXPECT_EQ(state.files[path.data()].size(), 2u);
    const std::array<uint8_t, 2> tail{'L', '1'};
    ASSERT_TRUE(resumed.write(2, tail));
    EXPECT_EQ(resumed.seal(), !corrupt);
    resumed.abort();
    EXPECT_EQ(state.files.contains(path.data()), !corrupt);
  }
  state.files[path.data()] = {'T'};
  HalTintaDerivedCandidateStage shortFile(scratch);
  EXPECT_FALSE(shortFile.resume(checkpoint, manifest, checkpoint.manifestHash, checkpoint.transaction, checkpoint.owner,
                                checkpoint.course, checkpoint.storage, pack, frontier));
  shortFile.abort();
  EXPECT_EQ(state.files[path.data()], (std::vector<uint8_t>{'T'}));
}
TEST_F(HalDictionaryArchiveStageTest, CheckpointedPartialCandidateSurvivesSessionExitAndResumesWithTailTrim) {
  constexpr char path[] = "/.crosspoint/companion/retained-test";
  for (bool lostClose : {false, true}) {
    inventory_hal_test::state.files.clear();
    {
      HalVerifiedFileStage pending(scratch);
      ASSERT_TRUE(pending.begin(path, BYTES.size()));
      ASSERT_TRUE(pending.write(0, std::span(BYTES).first(3)));
      uint64_t durable = 0;
      ASSERT_TRUE(pending.syncPending(durable));
      ASSERT_EQ(durable, 3u);
      ASSERT_TRUE(pending.write(3, std::span(BYTES).subspan(3)));
      inventory_hal_test::state.failClose = lostClose;
      EXPECT_EQ(pending.retainPending(durable), !lostClose);
      inventory_hal_test::state.failClose = false;
    }
    ASSERT_TRUE(inventory_hal_test::state.files.contains(path));
    HalVerifiedFileStage restarted(scratch);
    ASSERT_TRUE(restarted.resume(path, BYTES.size(), 3));
    EXPECT_EQ(inventory_hal_test::state.files[path].size(), 3u);
    ASSERT_TRUE(restarted.write(3, std::span(BYTES).subspan(3)));
    ASSERT_TRUE(restarted.seal(BYTES.size()));
    EXPECT_EQ(inventory_hal_test::state.files[path], (std::vector<uint8_t>(BYTES.begin(), BYTES.end())));
  }
  inventory_hal_test::state.files.clear();
  HalVerifiedFileStage unsynced(scratch);
  ASSERT_TRUE(unsynced.begin(path, BYTES.size()));
  ASSERT_TRUE(unsynced.write(0, BYTES));
  EXPECT_FALSE(unsynced.retainPending(BYTES.size()));
  unsynced.abort();
  EXPECT_FALSE(inventory_hal_test::state.files.contains(path));
}
TEST_F(HalDictionaryArchiveStageTest, PendingSyncReportsOffsetOnlyOnSuccessAndBlocksAmbiguousContinuation) {
  HalVerifiedFileStage pending(scratch);
  constexpr char path[] = "/.crosspoint/companion/pending-test";
  ASSERT_TRUE(pending.begin(path, BYTES.size()));
  ASSERT_TRUE(pending.write(0, std::span(BYTES).first(3)));
  uint64_t durable = 99;
  ASSERT_TRUE(pending.syncPending(durable));
  EXPECT_EQ(durable, 3u);
  ASSERT_TRUE(pending.write(3, std::span(BYTES).subspan(3)));
  inventory_hal_test::state.failSync = true;
  EXPECT_FALSE(pending.syncPending(durable));
  EXPECT_EQ(durable, 3u);
  inventory_hal_test::state.failSync = false;
  EXPECT_FALSE(pending.write(BYTES.size(), {}));
  EXPECT_FALSE(pending.seal(BYTES.size()));
  pending.abort();
  EXPECT_FALSE(inventory_hal_test::state.files.contains(path));
}
TEST_F(HalDictionaryArchiveStageTest, SealsOnlyAfterSyncedBytesMatchWriteStreamHash) {
  ASSERT_TRUE(stage.begin());
  ASSERT_TRUE(stage.write(0, BYTES));
  ASSERT_TRUE(stage.seal(BYTES.size()));
  EXPECT_TRUE(stage.isSealed());
  EXPECT_EQ(inventory_hal_test::state.files.at(stage.CANDIDATE), (std::vector<uint8_t>(BYTES.begin(), BYTES.end())));
  HalFile file;
  ASSERT_TRUE(Storage.openFileForRead("TEST", stage.CANDIDATE, file));
  Digest expected;
  uint64_t length;
  ASSERT_TRUE(hashInventoryFile(file, scratch, length, expected));
  EXPECT_EQ(stage.contentHash(), expected);
  stage.abort();
  EXPECT_TRUE(inventory_hal_test::state.files.contains(stage.CANDIDATE));
  EXPECT_FALSE(stage.begin());
  EXPECT_EQ(inventory_hal_test::state.files.at(stage.CANDIDATE).size(), BYTES.size());
}
TEST_F(HalDictionaryArchiveStageTest, RefusesUnknownCandidateAndPreservesSealedCandidateOnDestruction) {
  inventory_hal_test::state.files[stage.CANDIDATE] = {1, 2, 3};
  EXPECT_FALSE(stage.begin());
  stage.abort();
  EXPECT_EQ(inventory_hal_test::state.files.at(stage.CANDIDATE), (std::vector<uint8_t>{1, 2, 3}));
  inventory_hal_test::state.files.clear();
  inventory_hal_test::state.directories[stage.CANDIDATE] = {};
  EXPECT_FALSE(stage.begin());
  stage.abort();
  EXPECT_TRUE(inventory_hal_test::state.directories.contains(stage.CANDIDATE));
  inventory_hal_test::state.directories.clear();
  {
    HalDictionaryArchiveStage scoped(scratch);
    ASSERT_TRUE(scoped.begin());
    ASSERT_TRUE(scoped.write(0, BYTES));
    ASSERT_TRUE(scoped.seal(BYTES.size()));
  }
  EXPECT_TRUE(inventory_hal_test::state.files.contains(stage.CANDIDATE));
}
TEST_F(HalDictionaryArchiveStageTest, RejectsCorruptionAndSealFailuresBeforeHandoff) {
  for (unsigned fault = 0; fault < 5; ++fault) {
    inventory_hal_test::state = {};
    ASSERT_TRUE(stage.begin());
    if (fault == 0) inventory_hal_test::state.corruptWrite = true;
    ASSERT_TRUE(stage.write(0, BYTES));
    if (fault == 1) inventory_hal_test::state.failSync = true;
    if (fault == 2) inventory_hal_test::state.failTruncate = true;
    if (fault == 3) inventory_hal_test::state.failClose = true;
    if (fault == 4) inventory_hal_test::state.shortRead = inventory_hal_test::state.reads + 1;
    EXPECT_FALSE(stage.seal(BYTES.size())) << fault;
    EXPECT_FALSE(stage.isSealed());
    inventory_hal_test::state.failSync = inventory_hal_test::state.failTruncate = inventory_hal_test::state.failClose =
        false;
    stage.abort();
    EXPECT_FALSE(inventory_hal_test::state.files.contains(stage.CANDIDATE));
  }
}
TEST_F(HalDictionaryArchiveStageTest, EnforcesContiguousWritesAndRetainsWrapperAcrossBuilds) {
  ASSERT_TRUE(stage.begin());
  EXPECT_FALSE(stage.write(1, BYTES));
  stage.abort();
  EXPECT_FALSE(inventory_hal_test::state.files.contains(stage.CANDIDATE));
  for (unsigned build = 0; build < 5; ++build) {
    ASSERT_TRUE(stage.begin());
    for (unsigned write = 0; write < 32; ++write) ASSERT_TRUE(stage.write(write * BYTES.size(), BYTES));
    ASSERT_TRUE(stage.seal(32 * BYTES.size()));
    ASSERT_TRUE(Storage.remove(stage.CANDIDATE));
  }
  EXPECT_EQ(inventory_hal_test::state.preparations, 3u);
  EXPECT_GE(inventory_hal_test::state.yields, 5u);
}
TEST_F(HalDictionaryArchiveStageTest, CleanupFailureRetainsOwnershipForRetry) {
  ASSERT_TRUE(stage.begin());
  ASSERT_TRUE(stage.write(0, BYTES));
  inventory_hal_test::state.failRemove = true;
  stage.abort();
  EXPECT_TRUE(inventory_hal_test::state.files.contains(stage.CANDIDATE));
  EXPECT_FALSE(stage.begin());
  inventory_hal_test::state.failRemove = false;
  ASSERT_TRUE(stage.begin());
  ASSERT_TRUE(stage.write(0, BYTES));
  ASSERT_TRUE(stage.seal(BYTES.size()));
}
TEST_F(HalDictionaryArchiveStageTest, ActualMemberValidationAndArchiveBuilderProduceSealedCandidate) {
  auto& files = inventory_hal_test::state.files;
  files["/dictionaries/es/stem.dict"] = {'o', 'n', 'e', 't', 'w', 'o'};
  files["/dictionaries/es/stem.idx"] = fixture("DictionaryIndex-definitions.fixture");
  files["/dictionaries/es/stem.ifo"] = fixture("DictionaryInfo.fixture");
  files["/dictionaries/es/stem.syn"] = fixture("DictionaryIndex-synonyms.fixture");
  HalDictionaryBundleSource source;
  ASSERT_TRUE(source.begin("/dictionaries/es/stem", false, true));
  DictionaryMembersValidation validation(source, scratch);
  DictionaryMembersDetails details;
  ASSERT_TRUE(validation.validate(false, true, details));
  ASSERT_TRUE(source.reopen());
  DictionaryBundleBuilder builder(stage, scratch);
  uint64_t length = 99;
  ASSERT_TRUE(builder.build(source, false, true, length));
  EXPECT_TRUE(stage.isSealed());
  EXPECT_EQ(length, files.at(stage.CANDIDATE).size());
  EXPECT_EQ(files.at(stage.CANDIDATE)[0], 'P');
  EXPECT_EQ(files.at(stage.CANDIDATE)[1], 'K');
  EXPECT_EQ(inventory_hal_test::state.preparations, 7u);
}
TEST_F(HalDictionaryArchiveStageTest, PreparationOpenAndWriteErrorsDoNotSealOrLeakCandidates) {
  auto& state = inventory_hal_test::state;
  state.failDirectory = true;
  EXPECT_FALSE(stage.begin());
  state.failDirectory = false;
  state.failOpen = true;
  EXPECT_FALSE(stage.begin());
  stage.abort();
  EXPECT_FALSE(state.files.contains(stage.CANDIDATE));
  state.failOpen = false;
  ASSERT_TRUE(stage.begin());
  state.failWrite = true;
  EXPECT_FALSE(stage.write(0, BYTES));
  EXPECT_FALSE(stage.seal(BYTES.size()));
  stage.abort();
  EXPECT_FALSE(state.files.contains(stage.CANDIDATE));
  state.failWrite = false;
  HalDictionaryArchiveStage shortScratch{std::span(scratch).first(63)};
  EXPECT_FALSE(shortScratch.begin());
  EXPECT_FALSE(state.files.contains(stage.CANDIDATE));
}
TEST_F(HalDictionaryArchiveStageTest, CancellationDuringReadbackPreventsSealedHandoff) {
  unsigned calls = 0;
  HalDictionaryArchiveStage cancelled(
      scratch, [](void* context) { return ++*static_cast<unsigned*>(context) < 6; }, &calls);
  ASSERT_TRUE(cancelled.begin());
  const std::array<uint8_t, 128> bytes{};
  ASSERT_TRUE(cancelled.write(0, bytes));
  EXPECT_FALSE(cancelled.seal(bytes.size()));
  EXPECT_FALSE(cancelled.isSealed());
  EXPECT_EQ(inventory_hal_test::state.reads, 1u);
  cancelled.abort();
  EXPECT_FALSE(inventory_hal_test::state.files.contains(stage.CANDIDATE));
}
}  // namespace
