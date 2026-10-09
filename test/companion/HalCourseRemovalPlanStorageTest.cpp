#include <gtest/gtest.h>
#include <openssl/evp.h>

#include "lib/hal/HalCourseRemovalPlanStorage.h"

using namespace companion;
namespace {
class CoursePlanStorageTest : public testing::Test {
 protected:
  std::array<uint8_t, REMOVAL_STAGE_CLAIM_SIZE> comparison{};
  std::array<uint8_t, COURSE_REMOVAL_PLAN_SIZE> bytes{}, loaded{};
  CourseRemovalPlan plan;
  Digest hash{};
  HalCourseRemovalPlanStorage storage{comparison};
  void SetUp() override {
    auto& state = inventory_hal_test::state;
    state = {};
    state.enumerateFileMap = state.falseExists = true;
    plan.request.transaction.fill(1);
    plan.request.owner.fill(2);
    plan.request.generation.fill(3);
    plan.request.manifest.contentHash.fill(4);
    plan.request.manifest.logicalIdentity.fill(5);
    plan.request.manifest.kind = ContentKind::Course;
    plan.request.manifest.formatVersion = 1;
    plan.request.manifest.length = 1000;
    ASSERT_EQ(CourseRemovalPlanCodec::encode(plan, bytes), bytes.size());
    ASSERT_TRUE(EVP_Digest(bytes.data(), bytes.size(), hash.data(), nullptr, EVP_sha256(), nullptr));
  }
  std::string target() {
    std::string path = "/.crosspoint/companion/removal-course-plan-";
    constexpr char HEX[] = "0123456789abcdef";
    for (auto byte : hash) {
      path += HEX[byte >> 4];
      path += HEX[byte & 15];
    }
    return path;
  }
  std::string stage() {
    HalRemovalStageClaimStorage claims(comparison);
    EXPECT_EQ(claims.persist({plan.request, 9}), RemovalStageClaimResult::Ok);
    return claims.planStagePath() ? claims.planStagePath() : "";
  }
};
}  // namespace
TEST_F(CoursePlanStorageTest, ImmutablePublicationAndBootLoadRetainExactPlan) {
  Digest output{};
  ASSERT_EQ(storage.publish(bytes, 9, output), CourseRemovalPlanStorageResult::Ok);
  EXPECT_EQ(output, hash);
  EXPECT_STREQ(storage.publishedPath(), target().c_str());
  const auto files = inventory_hal_test::state.files;
  const auto renames = inventory_hal_test::state.renames;
  HalCourseRemovalPlanStorage rebooted(comparison);
  CourseRemovalPlan decoded;
  for (unsigned repeat = 0; repeat < 3; ++repeat) {
    EXPECT_EQ(rebooted.load(hash, loaded, decoded), CourseRemovalPlanStorageResult::Ok);
    EXPECT_EQ(decoded, plan);
    EXPECT_EQ(loaded, bytes);
    EXPECT_EQ(rebooted.publish(bytes, 9, output), CourseRemovalPlanStorageResult::Ok);
  }
  EXPECT_EQ(inventory_hal_test::state.files, files);
  EXPECT_EQ(inventory_hal_test::state.renames, renames);
}
TEST_F(CoursePlanStorageTest, ClaimedPartialStageRecoversButValidForeignStageIsPreserved) {
  const auto path = stage();
  auto& state = inventory_hal_test::state;
  auto foreign = plan;
  foreign.request.owner.fill(8);
  std::array<uint8_t, COURSE_REMOVAL_PLAN_SIZE> foreignBytes;
  ASSERT_EQ(CourseRemovalPlanCodec::encode(foreign, foreignBytes), foreignBytes.size());
  state.files[path] = {foreignBytes.begin(), foreignBytes.end()};
  const auto before = state.files;
  Digest output{};
  EXPECT_EQ(storage.publish(bytes, 9, output), CourseRemovalPlanStorageResult::Conflict);
  EXPECT_EQ(state.files, before);
  EXPECT_EQ(output, Digest{});
  state.files[path] = {1, 2, 3};
  EXPECT_EQ(storage.publish(bytes, 9, output), CourseRemovalPlanStorageResult::Ok);
  EXPECT_EQ(output, hash);
}
TEST_F(CoursePlanStorageTest, InterruptedPlanRenameRecoversBeforeAndAfterEffect) {
  for (bool after : {false, true}) {
    SetUp();
    stage();
    auto& state = inventory_hal_test::state;
    if (after)
      state.failRenameAfter = state.renames + 1;
    else
      state.failRename = state.renames + 1;
    Digest output{};
    EXPECT_EQ(storage.publish(bytes, 9, output), CourseRemovalPlanStorageResult::IoError);
    EXPECT_EQ(output, Digest{});
    state.failRename = state.failRenameAfter = 0;
    HalCourseRemovalPlanStorage rebooted(comparison);
    EXPECT_EQ(rebooted.publish(bytes, 9, output), CourseRemovalPlanStorageResult::Ok);
    EXPECT_EQ(output, hash);
  }
}
TEST_F(CoursePlanStorageTest, CorruptPublishedBytesAndDirectoryCollisionsStayUntouched) {
  for (bool directory : {false, true}) {
    SetUp();
    auto& state = inventory_hal_test::state;
    if (directory)
      state.directories[target()] = {};
    else
      state.files[target()] = {1, 2, 3};
    const auto before = state.files;
    Digest output{};
    EXPECT_NE(storage.publish(bytes, 9, output), CourseRemovalPlanStorageResult::Ok);
    EXPECT_EQ(state.files, before);
    EXPECT_EQ(state.renames, 0U);
    EXPECT_EQ(output, Digest{});
  }
}
TEST_F(CoursePlanStorageTest, InvalidAdmissionAndLoadFailureKeepCallerOutputs) {
  Digest output{};
  EXPECT_EQ(storage.publish(bytes, 0, output), CourseRemovalPlanStorageResult::Invalid);
  std::array<uint8_t, REMOVAL_STAGE_CLAIM_SIZE> overlappingBytes{};
  std::copy(bytes.begin(), bytes.end(), overlappingBytes.begin());
  HalCourseRemovalPlanStorage overlap(overlappingBytes);
  EXPECT_EQ(overlap.publish(std::span(overlappingBytes).first(bytes.size()), 9, output),
            CourseRemovalPlanStorageResult::Invalid);
  EXPECT_TRUE(inventory_hal_test::state.files.empty());
  ASSERT_EQ(storage.publish(bytes, 9, output), CourseRemovalPlanStorageResult::Ok);
  auto decoded = plan;
  decoded.request.owner.fill(8);
  const auto unchanged = decoded;
  auto& state = inventory_hal_test::state;
  state.shortRead = state.reads + 1;
  EXPECT_EQ(storage.load(hash, loaded, decoded), CourseRemovalPlanStorageResult::IoError);
  EXPECT_EQ(decoded, unchanged);
  state.shortRead = 0;
  state.files[target()].back() ^= 1;
  EXPECT_EQ(storage.load(hash, loaded, decoded), CourseRemovalPlanStorageResult::Corrupt);
  EXPECT_EQ(decoded, unchanged);
}
