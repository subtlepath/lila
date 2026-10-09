#include <gtest/gtest.h>

#include "lib/hal/HalCourseRemovalAdmission.h"
using namespace companion;
namespace {
struct Snapshot final : InventoryIndexStorage {
  std::vector<uint8_t> bytes;
  bool fail = false;
  bool size(uint64_t& output) override {
    output = bytes.size();
    return !fail;
  }
  bool read(uint64_t offset, std::span<uint8_t> output) override {
    if (fail || offset > bytes.size() || output.size() > bytes.size() - offset) return false;
    std::copy_n(bytes.begin() + offset, output.size(), output.begin());
    return true;
  }
};
class CourseAdmissionTest : public testing::Test {
 protected:
  std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> journalBytes{};
  std::array<uint8_t, INVENTORY_PATH_MAX_RECORD> pathBytes{};
  std::array<uint8_t, REMOVAL_STAGE_CLAIM_SIZE> comparison{};
  HalContentRemovalJournalStorage journalStorage;
  ContentRemovalJournal journal{journalStorage, journalBytes};
  Snapshot snapshot;
  InventoryPaths paths{snapshot, pathBytes};
  HalCourseRemovalPlanStorage plans{comparison};
  Identity generation = [] {
    Identity value{};
    value.fill(3);
    return value;
  }();
  ContentRemovalRequest request;
  bool allowed = true, inventoryOk = true, stateOk = true;
  unsigned inventories = 0, preparations = 0;
  static bool permission(void* ctx) { return static_cast<CourseAdmissionTest*>(ctx)->allowed; }
  static bool inventory(void* ctx, uint64_t& revision) {
    auto& self = *static_cast<CourseAdmissionTest*>(ctx);
    ++self.inventories;
    revision = 9;
    return self.inventoryOk;
  }
  static bool prepareState(void* ctx, const ContentManifest& manifest) {
    auto& self = *static_cast<CourseAdmissionTest*>(ctx);
    ++self.preparations;
    EXPECT_FALSE(self.journal.current());
    EXPECT_EQ(manifest, self.request.manifest);
    return self.stateOk;
  }
  HalCourseRemovalAdmission admission{generation, journal, paths, plans, inventory, prepareState, permission, this};
  void SetUp() override {
    inventory_hal_test::state = {};
    inventory_hal_test::state.enumerateFileMap = true;
    request.transaction.fill(1);
    request.owner.fill(2);
    request.generation.fill(3);
    request.manifest.kind = ContentKind::Course;
    request.manifest.length = 123;
    request.manifest.formatVersion = 1;
    request.manifest.logicalIdentity.fill(4);
    request.manifest.contentHash.fill(5);
    makeSnapshot(request.manifest, {ACTIVE_COURSE_PATH});
  }
  void makeSnapshot(const ContentManifest& manifest, std::initializer_list<const char*> names) {
    snapshot.bytes.resize(INVENTORY_INDEX_HEADER_SIZE + names.size() * INVENTORY_PATH_MAX_RECORD);
    size_t used = INVENTORY_INDEX_HEADER_SIZE;
    for (const auto* name : names) used += encodeInventoryPath(manifest, name, std::span(snapshot.bytes).subspan(used));
    snapshot.bytes.resize(used);
    InventoryIndexHeader header{request.generation, 9, names.size(),
                                inventoryIndexCrc(std::span(snapshot.bytes).subspan(INVENTORY_INDEX_HEADER_SIZE))};
    ASSERT_EQ(encodeInventoryPathsHeader(header, snapshot.bytes), INVENTORY_INDEX_HEADER_SIZE);
  }
};
}  // namespace
TEST_F(CourseAdmissionTest, FreshAdmissionSealsExactInventoryAfterStatePreparation) {
  ContentRemovalRecord output;
  ASSERT_EQ(admission.admit(request, 0, output), EpubRemovalAdmissionResult::Ready);
  EXPECT_EQ(output.request, request);
  EXPECT_EQ(output.phase, ContentRemovalPhase::Prepared);
  EXPECT_EQ(output.revision, 1u);
  EXPECT_NE(output.planHash, Digest{});
  EXPECT_EQ(inventories, 1u);
  EXPECT_EQ(preparations, 1u);
  EXPECT_FALSE(journal.current());
  ASSERT_TRUE(plans.publishedPath());
}
TEST_F(CourseAdmissionTest, RetainedOwnerResumesWithoutInventoryOrStatePreparation) {
  ContentRemovalRecord expected, output;
  ASSERT_EQ(admission.admit(request, 9, expected), EpubRemovalAdmissionResult::Ready);
  ASSERT_EQ(journal.begin(expected), ContentRemovalJournalResult::Ok);
  snapshot.fail = true;
  inventoryOk = stateOk = false;
  ASSERT_EQ(admission.admit(request, 0, output), EpubRemovalAdmissionResult::Ready);
  EXPECT_EQ(output, expected);
  EXPECT_EQ(inventories, 1u);
  EXPECT_EQ(preparations, 1u);
  auto other = request;
  other.transaction[0] ^= 1;
  EXPECT_EQ(admission.admit(other, 9, output), EpubRemovalAdmissionResult::Busy);
  EXPECT_EQ(output, expected);
  for (const auto phase :
       {ContentRemovalPhase::Quarantined, ContentRemovalPhase::Committed, ContentRemovalPhase::Retired})
    ASSERT_EQ(journal.advance(phase), ContentRemovalJournalResult::Ok);
  EXPECT_EQ(admission.admit(request, 0, output), EpubRemovalAdmissionResult::Retired);
  EXPECT_EQ(output, expected);
}
TEST_F(CourseAdmissionTest, MismatchedOrDuplicateInventoryNeverPreparesStateOrPublishesPlan) {
  ContentRemovalRecord output;
  output.planHash.fill(99);
  const auto unchanged = output;
  auto other = request.manifest;
  ++other.length;
  makeSnapshot(other, {ACTIVE_COURSE_PATH});
  EXPECT_EQ(admission.admit(request, 9, output), EpubRemovalAdmissionResult::Conflict);
  makeSnapshot(request.manifest, {"/other/course.pack"});
  EXPECT_EQ(admission.admit(request, 9, output), EpubRemovalAdmissionResult::Conflict);
  makeSnapshot(request.manifest, {ACTIVE_COURSE_PATH, ACTIVE_COURSE_PATH});
  EXPECT_EQ(admission.admit(request, 9, output), EpubRemovalAdmissionResult::Conflict);
  other.contentHash[0] ^= 1;
  makeSnapshot(other, {ACTIVE_COURSE_PATH});
  EXPECT_EQ(admission.admit(request, 9, output), EpubRemovalAdmissionResult::NotFound);
  EXPECT_EQ(preparations, 0u);
  EXPECT_EQ(output, unchanged);
  EXPECT_TRUE(inventory_hal_test::state.files.empty());
}
TEST_F(CourseAdmissionTest, UnauthorizedWrongCardAndFailedPreparationLeaveOutputUnchanged) {
  ContentRemovalRecord output;
  output.planHash.fill(99);
  const auto unchanged = output;
  allowed = false;
  EXPECT_EQ(admission.admit(request, 9, output), EpubRemovalAdmissionResult::Busy);
  allowed = true;
  auto wrong = request;
  wrong.generation[0] ^= 1;
  EXPECT_EQ(admission.admit(wrong, 9, output), EpubRemovalAdmissionResult::WrongStorage);
  stateOk = false;
  EXPECT_EQ(admission.admit(request, 9, output), EpubRemovalAdmissionResult::IoError);
  EXPECT_EQ(output, unchanged);
  EXPECT_FALSE(journal.current());
  EXPECT_TRUE(inventory_hal_test::state.files.empty());
}
TEST_F(CourseAdmissionTest, InterruptedPublicationRetriesAndCorruptRetainedPlanCannotAuthorizeRemoval) {
  ContentRemovalRecord output;
  output.planHash.fill(99);
  const auto unchanged = output;
  inventory_hal_test::state.failRename = 1;
  EXPECT_EQ(admission.admit(request, 9, output), EpubRemovalAdmissionResult::IoError);
  EXPECT_EQ(output, unchanged);
  EXPECT_FALSE(journal.current());
  inventory_hal_test::state.failRename = 0;
  ASSERT_EQ(admission.admit(request, 9, output), EpubRemovalAdmissionResult::Ready);
  ASSERT_EQ(journal.begin(output), ContentRemovalJournalResult::Ok);
  const std::string path(plans.publishedPath());
  inventory_hal_test::state.files[path][0] ^= 1;
  const auto before = inventory_hal_test::state.files;
  output = unchanged;
  EXPECT_EQ(admission.admit(request, 9, output), EpubRemovalAdmissionResult::Corrupt);
  EXPECT_EQ(output, unchanged);
  EXPECT_EQ(inventory_hal_test::state.files, before);
  EXPECT_TRUE(journal.current());
}
