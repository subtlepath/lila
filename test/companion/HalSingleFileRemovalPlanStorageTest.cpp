#include <gtest/gtest.h>
#include <openssl/evp.h>

#include "lib/hal/HalSingleFileRemovalPlanStorage.h"
using namespace companion;
namespace {
class HalRemovalPlanTest : public testing::Test {
 protected:
  std::array<uint8_t, 128> comparison{};
  std::array<uint8_t, SINGLE_FILE_REMOVAL_PLAN_MAX_SIZE> bytes{}, loaded{};
  HalSingleFileRemovalPlanStorage storage{comparison};
  size_t length = 0;
  Digest expected{};
  ContentRemovalRequest request;
  void SetUp() override {
    inventory_hal_test::state = {};
    inventory_hal_test::state.enumerateFileMap = true;
    request.transaction.fill(1);
    request.owner.fill(2);
    request.generation.fill(3);
    request.manifest.contentHash.fill(4);
    request.manifest.kind = ContentKind::Epub;
    request.manifest.length = 1234;
    request.manifest.formatVersion = 1;
    length = encodeSingleFileRemovalPlan({request, "/Books/caf\xc3\xa9.epub"}, bytes);
    ASSERT_GT(length, 0);
    EVP_Digest(bytes.data(), length, expected.data(), nullptr, EVP_sha256(), nullptr);
  }
  std::string path() {
    std::string result = "/.crosspoint/companion/removal-plan-";
    result.reserve(100);
    constexpr char HEX[] = "0123456789abcdef";
    for (auto byte : expected) {
      result += HEX[byte >> 4];
      result += HEX[byte & 15];
    }
    return result;
  }
};
}  // namespace
TEST_F(HalRemovalPlanTest, VerifiedPublicationAndBootLoadReuseHandlesAndExactBytes) {
  Digest output{};
  ASSERT_EQ(storage.publish(std::span(bytes).first(length), output), RemovalPlanStorageResult::Ok);
  EXPECT_EQ(output, expected);
  EXPECT_EQ(storage.publishedPath(), path());
  EXPECT_EQ(inventory_hal_test::state.files.at(path()), std::vector<uint8_t>(bytes.begin(), bytes.begin() + length));
  const auto wrappers = inventory_hal_test::state.preparations;
  inventory_hal_test::state.falseExists = true;
  for (unsigned repeat = 0; repeat < 5; ++repeat) {
    EXPECT_EQ(storage.publish(std::span(bytes).first(length), output), RemovalPlanStorageResult::Ok);
    SingleFileRemovalPlan plan;
    ASSERT_EQ(storage.load(expected, loaded, plan), RemovalPlanStorageResult::Ok);
    EXPECT_EQ(plan.request, request);
    EXPECT_EQ(plan.path, "/Books/caf\xc3\xa9.epub");
    EXPECT_EQ(inventory_hal_test::state.preparations, wrappers);
  }
  HalSingleFileRemovalPlanStorage rebooted(comparison);
  SingleFileRemovalPlan plan;
  EXPECT_EQ(rebooted.load(expected, loaded, plan), RemovalPlanStorageResult::Ok);
  EXPECT_EQ(plan.request, request);
}
TEST_F(HalRemovalPlanTest, InterruptedRenameResumesFromSealedStageOrPublishedTarget) {
  for (bool after : {false, true}) {
    inventory_hal_test::state = {};
    auto& state = inventory_hal_test::state;
    state.enumerateFileMap = true;
    state.failRename = after ? 0 : 1;
    state.failRenameAfter = after ? 1 : 0;
    HalSingleFileRemovalPlanStorage local(comparison);
    Digest output{};
    EXPECT_EQ(local.publish(std::span(bytes).first(length), output), RemovalPlanStorageResult::IoError);
    EXPECT_EQ(output, Digest{});
    EXPECT_EQ(local.publishedPath(), nullptr);
    state.failRename = state.failRenameAfter = 0;
    HalSingleFileRemovalPlanStorage reopened(comparison);
    ASSERT_EQ(reopened.publish(std::span(bytes).first(length), output), RemovalPlanStorageResult::Ok);
    EXPECT_EQ(output, expected);
    EXPECT_EQ(state.files.at(path()), std::vector<uint8_t>(bytes.begin(), bytes.begin() + length));
  }
}
TEST_F(HalRemovalPlanTest, StageWriteTruncateSyncCloseAndCorruptionCannotExposeAPlan) {
  for (unsigned mode = 0; mode < 5; ++mode) {
    inventory_hal_test::state = {};
    auto& state = inventory_hal_test::state;
    state.enumerateFileMap = true;
    state.failWrite = mode == 0;
    state.failTruncate = mode == 1;
    state.failSync = mode == 2;
    state.failClose = mode == 3;
    state.corruptWrite = mode == 4;
    HalSingleFileRemovalPlanStorage local(comparison);
    Digest output{};
    const auto result = local.publish(std::span(bytes).first(length), output);
    EXPECT_NE(result, RemovalPlanStorageResult::Ok);
    EXPECT_EQ(output, Digest{});
    EXPECT_EQ(local.publishedPath(), nullptr);
    EXPECT_FALSE(state.files.contains(path()));
    state.failWrite = state.failTruncate = state.failSync = state.failClose = state.corruptWrite = false;
    EXPECT_EQ(local.publish(std::span(bytes).first(length), output), RemovalPlanStorageResult::Ok);
  }
}
TEST_F(HalRemovalPlanTest, ExistingCorruptTargetsAndDirectoryCollisionsRemainUntouched) {
  for (bool directory : {false, true}) {
    inventory_hal_test::state = {};
    auto& state = inventory_hal_test::state;
    state.enumerateFileMap = true;
    if (directory)
      state.directories[path()] = {};
    else
      state.files[path()] = {1, 2, 3};
    const auto before = state.files;
    Digest output{};
    EXPECT_NE(storage.publish(std::span(bytes).first(length), output), RemovalPlanStorageResult::Ok);
    EXPECT_EQ(output, Digest{});
    EXPECT_EQ(state.files, before);
    EXPECT_EQ(state.renames, 0);
    EXPECT_EQ(storage.publishedPath(), nullptr);
  }
}
TEST_F(HalRemovalPlanTest, AdmissionAndLoadFailuresPreserveOutputs) {
  Digest output{};
  EXPECT_EQ(storage.publish(std::span(bytes).first(length - 1), output), RemovalPlanStorageResult::Invalid);
  HalSingleFileRemovalPlanStorage overlapping{std::span(bytes).first(128)};
  EXPECT_EQ(overlapping.publish(std::span(bytes).first(length), output), RemovalPlanStorageResult::Invalid);
  EXPECT_TRUE(inventory_hal_test::state.files.empty());
  SingleFileRemovalPlan plan{request, "/Books/original.epub"};
  EXPECT_EQ(storage.load(Digest{}, loaded, plan), RemovalPlanStorageResult::Invalid);
  EXPECT_EQ(storage.load(expected, std::span(loaded).first(loaded.size() - 1), plan),
            RemovalPlanStorageResult::Invalid);
  EXPECT_EQ(plan.path, "/Books/original.epub");
  EXPECT_EQ(storage.load(expected, loaded, plan), RemovalPlanStorageResult::Missing);
  ASSERT_EQ(storage.publish(std::span(bytes).first(length), output), RemovalPlanStorageResult::Ok);
  auto& state = inventory_hal_test::state;
  state.shortRead = state.reads + 1;
  EXPECT_EQ(storage.load(expected, loaded, plan), RemovalPlanStorageResult::IoError);
  EXPECT_EQ(plan.path, "/Books/original.epub");
  state.shortRead = 0;
  state.files[path()].back() ^= 1;
  EXPECT_EQ(storage.load(expected, loaded, plan), RemovalPlanStorageResult::Corrupt);
  EXPECT_EQ(plan.path, "/Books/original.epub");
}
TEST_F(HalRemovalPlanTest, RecoveredValidBytesRequireSyncBeforeReturningSuccess) {
  Digest output{};
  ASSERT_EQ(storage.publish(std::span(bytes).first(length), output), RemovalPlanStorageResult::Ok);
  auto& state = inventory_hal_test::state;
  state.failSync = true;
  output.fill(9);
  const auto saved = output;
  EXPECT_EQ(storage.publish(std::span(bytes).first(length), output), RemovalPlanStorageResult::IoError);
  EXPECT_EQ(output, saved);
  SingleFileRemovalPlan plan{request, "/Books/original.epub"};
  EXPECT_EQ(storage.load(expected, loaded, plan), RemovalPlanStorageResult::IoError);
  EXPECT_EQ(plan.path, "/Books/original.epub");
  state.failSync = false;
  EXPECT_EQ(storage.load(expected, loaded, plan), RemovalPlanStorageResult::Ok);
}

TEST_F(HalRemovalPlanTest, UnpublishedStageCollisionsAndDirectoryErrorsDoNotModifyFiles) {
  for (unsigned mode = 0; mode < 3; ++mode) {
    inventory_hal_test::state = {};
    auto& state = inventory_hal_test::state;
    state.enumerateFileMap = true;
    if (mode == 0) state.directories[path() + ".tmp"] = {};
    if (mode == 1) state.files[path() + ".tmp"].resize(SINGLE_FILE_REMOVAL_PLAN_MAX_SIZE + 1);
    if (mode == 2) state.directoryErrorPath = TRANSFER_DIRECTORY;
    const auto before = state.files;
    Digest output{};
    EXPECT_EQ(storage.publish(std::span(bytes).first(length), output), RemovalPlanStorageResult::IoError);
    EXPECT_EQ(state.files, before);
    EXPECT_EQ(state.renames, 0);
    EXPECT_EQ(output, Digest{});
  }
}
TEST_F(HalRemovalPlanTest, ValidCRCWithDifferentBytesCannotSatisfyJournalPlanHash) {
  Digest output{};
  ASSERT_EQ(storage.publish(std::span(bytes).first(length), output), RemovalPlanStorageResult::Ok);
  auto& altered = inventory_hal_test::state.files.at(path());
  altered[SINGLE_FILE_REMOVAL_PLAN_PREFIX + 7] = 'x';
  inventory_detail::write(altered, altered.size() - 4, inventoryIndexCrc(std::span(altered).first(altered.size() - 4)),
                          4);
  SingleFileRemovalPlan decoded;
  ASSERT_TRUE(decodeSingleFileRemovalPlan(altered, decoded));
  SingleFileRemovalPlan preserved{request, "/Books/original.epub"};
  EXPECT_EQ(storage.load(expected, loaded, preserved), RemovalPlanStorageResult::Corrupt);
  EXPECT_EQ(preserved.request, request);
  EXPECT_EQ(preserved.path, "/Books/original.epub");
  EXPECT_EQ(storage.publishedPath(), nullptr);
}
