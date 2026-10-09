#include <gtest/gtest.h>

#include "lib/hal/HalContentRemovalJournalStorage.h"
#include "lib/hal/HalCourseRemovalProofStorage.h"

using namespace companion;
namespace {
class CourseProofStorageTest : public testing::Test {
 protected:
  std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> journalBytes{}, proofBytes{};
  HalContentRemovalJournalStorage storage;
  ContentRemovalJournal journal{storage, journalBytes};
  HalCourseRemovalProofStorage proofs{proofBytes};
  ContentRemovalRecord seed, published;
  void SetUp() override {
    auto& state = inventory_hal_test::state;
    state = {};
    state.enumerateFileMap = state.falseExists = true;
    seed.request.transaction.fill(1);
    seed.request.owner.fill(2);
    seed.request.generation.fill(3);
    seed.request.manifest.kind = ContentKind::Course;
    seed.request.manifest.formatVersion = 1;
    seed.request.manifest.length = 1000;
    seed.request.manifest.contentHash.fill(4);
    seed.request.manifest.logicalIdentity.fill(5);
    seed.planHash.fill(6);
    ASSERT_EQ(journal.begin(seed), ContentRemovalJournalResult::Ok);
    ASSERT_EQ(journal.advance(ContentRemovalPhase::Quarantined), ContentRemovalJournalResult::Ok);
    published = *journal.current();
  }
  std::vector<uint8_t> encode(const ContentRemovalRecord& r) {
    std::vector<uint8_t> bytes(CONTENT_REMOVAL_RECORD_SIZE);
    EXPECT_EQ(encodeContentRemovalRecord(r, bytes), bytes.size());
    return bytes;
  }
  void interruptedRename(bool after) {
    auto& state = inventory_hal_test::state;
    if (after)
      state.failRenameAfter = state.renames + 1;
    else
      state.failRename = state.renames + 1;
    EXPECT_EQ(proofs.persist(published, journal), CourseRemovalProofStorageResult::IoError);
    state.failRename = state.failRenameAfter = 0;
    HalCourseRemovalProofStorage reopened(proofBytes);
    EXPECT_EQ(reopened.persist(published, journal), CourseRemovalProofStorageResult::Ok);
    ContentRemovalRecord loaded;
    ASSERT_EQ(reopened.load(seed.request.generation, loaded), CourseRemovalProofStorageResult::Ok);
    EXPECT_EQ(loaded, published);
  }
};
}  // namespace
TEST_F(CourseProofStorageTest, RequiresExactQuarantinedOwnerAndIsIdempotentAcrossReopen) {
  const auto before = inventory_hal_test::state.files;
  EXPECT_EQ(proofs.persist(seed, journal), CourseRemovalProofStorageResult::Invalid);
  auto foreign = published;
  foreign.request.owner.fill(8);
  EXPECT_EQ(proofs.persist(foreign, journal), CourseRemovalProofStorageResult::Invalid);
  EXPECT_EQ(inventory_hal_test::state.files, before);
  ASSERT_EQ(proofs.persist(published, journal), CourseRemovalProofStorageResult::Ok);
  const auto files = inventory_hal_test::state.files;
  const auto renames = inventory_hal_test::state.renames;
  HalCourseRemovalProofStorage reopened(proofBytes);
  EXPECT_EQ(reopened.persist(published, journal), CourseRemovalProofStorageResult::Ok);
  ContentRemovalRecord loaded;
  ASSERT_EQ(reopened.load(seed.request.generation, loaded), CourseRemovalProofStorageResult::Ok);
  EXPECT_EQ(loaded, published);
  Identity replaced;
  replaced.fill(9);
  EXPECT_EQ(reopened.load(replaced, loaded), CourseRemovalProofStorageResult::Conflict);
  EXPECT_EQ(loaded, published);
  EXPECT_EQ(inventory_hal_test::state.files, files);
  EXPECT_EQ(inventory_hal_test::state.renames, renames);
}
TEST_F(CourseProofStorageTest, InterruptedRenameBeforeEffectRecovers) { interruptedRename(false); }
TEST_F(CourseProofStorageTest, InterruptedRenameAfterEffectRecovers) { interruptedRename(true); }
TEST_F(CourseProofStorageTest, ForeignAndWrongPhaseStagesRemainUntouched) {
  auto& state = inventory_hal_test::state;
  for (bool wrongPhase : {false, true}) {
    auto foreign = wrongPhase ? seed : published;
    if (!wrongPhase) foreign.request.owner.fill(8);
    state.files[COURSE_REMOVAL_PROOF_STAGE] = encode(foreign);
    const auto before = state.files;
    EXPECT_EQ(proofs.persist(published, journal), CourseRemovalProofStorageResult::Conflict);
    EXPECT_EQ(state.files, before);
  }
  state.files[COURSE_REMOVAL_PROOF_STAGE] = {1, 2, 3};
  EXPECT_EQ(proofs.persist(published, journal), CourseRemovalProofStorageResult::Ok);
}
TEST_F(CourseProofStorageTest, CorruptPublishedProofIsPreservedAndLoadDoesNotChangeOutput) {
  auto& state = inventory_hal_test::state;
  state.files[COURSE_REMOVAL_PROOF_PATH] = {1, 2, 3};
  const auto before = state.files;
  EXPECT_EQ(proofs.persist(published, journal), CourseRemovalProofStorageResult::Corrupt);
  auto loaded = seed;
  EXPECT_EQ(proofs.load(seed.request.generation, loaded), CourseRemovalProofStorageResult::Corrupt);
  EXPECT_EQ(loaded, seed);
  EXPECT_EQ(state.files, before);
}
TEST_F(CourseProofStorageTest, SyncAndCloseFailuresCannotReturnVerifiedProof) {
  ASSERT_EQ(proofs.persist(published, journal), CourseRemovalProofStorageResult::Ok);
  auto& state = inventory_hal_test::state;
  for (bool close : {false, true}) {
    if (close)
      state.failClosePath = COURSE_REMOVAL_PROOF_PATH;
    else
      state.failSyncPath = COURSE_REMOVAL_PROOF_PATH;
    auto loaded = seed;
    EXPECT_EQ(proofs.load(seed.request.generation, loaded), CourseRemovalProofStorageResult::IoError);
    EXPECT_EQ(loaded, seed);
    state.failClosePath.clear();
    state.failSyncPath.clear();
  }
  EXPECT_TRUE(proofs.closeReaders());
}

TEST_F(CourseProofStorageTest, RevokedPublicationOwnerResumesAndNeverReturnsAnUncheckedProof) {
  struct Permission {
    unsigned calls = 0, revoke = 0;
    static bool check(void* context) {
      auto& permission = *static_cast<Permission*>(context);
      ++permission.calls;
      return !permission.revoke || permission.calls < permission.revoke;
    }
  } permission;
  auto& state = inventory_hal_test::state;
  const auto before = state;
  {
    HalCourseRemovalProofStorage guarded(proofBytes, Permission::check, &permission);
    ASSERT_EQ(guarded.persist(published, journal), CourseRemovalProofStorageResult::Ok);
  }
  const auto publicationChecks = permission.calls;
  for (unsigned revoke = 1; revoke <= publicationChecks; ++revoke) {
    state = before;
    permission = {0, revoke};
    HalCourseRemovalProofStorage guarded(proofBytes, Permission::check, &permission);
    EXPECT_NE(guarded.persist(published, journal), CourseRemovalProofStorageResult::Ok);
    ASSERT_TRUE(journal.current());
    EXPECT_EQ(*journal.current(), published);
    ASSERT_TRUE(guarded.closeReaders());
    HalCourseRemovalProofStorage reopened(proofBytes);
    ASSERT_EQ(reopened.persist(published, journal), CourseRemovalProofStorageResult::Ok);
    auto loaded = seed;
    ASSERT_EQ(reopened.load(seed.request.generation, loaded), CourseRemovalProofStorageResult::Ok);
    EXPECT_EQ(loaded, published);
  }
  permission = {};
  HalCourseRemovalProofStorage guarded(proofBytes, Permission::check, &permission);
  auto loaded = seed;
  ASSERT_EQ(guarded.load(seed.request.generation, loaded), CourseRemovalProofStorageResult::Ok);
  const auto loadChecks = permission.calls;
  const auto files = state.files;
  for (unsigned revoke = 1; revoke <= loadChecks; ++revoke) {
    permission = {0, revoke};
    loaded = seed;
    EXPECT_NE(guarded.load(seed.request.generation, loaded), CourseRemovalProofStorageResult::Ok);
    EXPECT_EQ(loaded, seed);
    EXPECT_EQ(state.files, files);
    EXPECT_TRUE(guarded.closeReaders());
  }
}
