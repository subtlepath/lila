#include <gtest/gtest.h>

#include "../../lib/hal/HalInventoryIndexStorage.h"
#include "../../lib/hal/HalInventoryPathsStage.h"
using namespace companion;
class PathStageTest : public testing::Test {
 protected:
  void SetUp() override { inventory_hal_test::state = {}; }
};
TEST_F(PathStageTest, BuildsDurableCandidateWithOneHandleAndRetainsSealedFile) {
  Identity generation{};
  generation[0] = 1;
  std::array<uint8_t, INVENTORY_PATH_MAX_RECORD> scratch;
  ContentManifest manifest;
  manifest.length = 123;
  manifest.contentHash[0] = 42;
  {
    HalInventoryPathsStage stage;
    InventoryPathsBuilder builder(stage, scratch);
    ASSERT_TRUE(builder.begin(generation, 4));
    for (unsigned i = 0; i < 64; ++i) ASSERT_TRUE(builder.record(manifest, "/books/book.epub"));
    ASSERT_TRUE(builder.seal());
    EXPECT_EQ(inventory_hal_test::state.opens, 1u);
    EXPECT_EQ(inventory_hal_test::state.closes, 1u);
    EXPECT_GE(inventory_hal_test::state.yields, 2u);
  }
  ASSERT_TRUE(inventory_hal_test::state.files.contains(HalInventoryPathsStage::CANDIDATE));
  HalInventoryIndexStorage reader;
  ASSERT_TRUE(reader.open(HalInventoryPathsStage::CANDIDATE));
  InventoryPaths paths(reader, scratch);
  ASSERT_TRUE(paths.open(generation, 4));
  std::array<char, 512> path;
  ASSERT_EQ(paths.find(manifest, path), InventoryPathResult::Found);
  EXPECT_STREQ(path.data(), "/books/book.epub");
}
TEST_F(PathStageTest, EveryBackendFailureAbortsCandidateAndCanRestart) {
  for (unsigned failure = 0; failure < 6; ++failure) {
    auto& state = inventory_hal_test::state;
    state = {};
    HalInventoryPathsStage stage;
    if (failure == 0) state.failOpen = true;
    if (failure == 1) state.failDirectory = true;
    if (failure < 2) {
      EXPECT_FALSE(stage.begin());
    } else {
      ASSERT_TRUE(stage.begin());
      const std::array<uint8_t, 3> bytes{1, 2, 3};
      if (failure == 2) state.failWrite = true;
      if (failure == 2)
        EXPECT_FALSE(stage.write(0, bytes));
      else {
        ASSERT_TRUE(stage.write(0, bytes));
        if (failure == 3) state.failTruncate = true;
        if (failure == 4) state.failSync = true;
        if (failure == 5) state.failClose = true;
        EXPECT_FALSE(stage.seal(3));
      }
    }
    state.failOpen = state.failDirectory = state.failWrite = state.failTruncate = state.failSync = state.failClose =
        false;
    stage.abort();
    EXPECT_FALSE(state.files.contains(HalInventoryPathsStage::CANDIDATE));
    EXPECT_GT(state.errors, 0u);
    ASSERT_TRUE(stage.begin());
    stage.abort();
  }
}
TEST_F(PathStageTest, FailedBoundsInvalidateStageAndDestructorCleansIncompleteCandidate) {
  auto& state = inventory_hal_test::state;
  {
    HalInventoryPathsStage stage;
    ASSERT_TRUE(stage.begin());
    const std::array<uint8_t, 3> bytes{1, 2, 3};
    EXPECT_FALSE(stage.write(1, bytes));
    EXPECT_FALSE(stage.write(0, bytes));
    EXPECT_FALSE(stage.seal(0));
  }
  EXPECT_FALSE(state.files.contains(HalInventoryPathsStage::CANDIDATE));
}
TEST_F(PathStageTest, HeaderRewriteAndExplicitRestartTruncateStaleCandidate) {
  HalInventoryPathsStage stage;
  ASSERT_TRUE(stage.begin());
  const std::array<uint8_t, 3> bytes{1, 2, 3};
  ASSERT_TRUE(stage.write(0, bytes));
  ASSERT_TRUE(stage.write(0, std::span(bytes).first(1)));
  EXPECT_FALSE(stage.seal(1));
  ASSERT_TRUE(stage.begin());
  ASSERT_TRUE(stage.write(0, std::span(bytes).first(1)));
  ASSERT_TRUE(stage.seal(1));
  EXPECT_EQ(inventory_hal_test::state.files.at(HalInventoryPathsStage::CANDIDATE), std::vector<uint8_t>{1});
}

TEST_F(PathStageTest, CleanupFailureRetainsCandidateAndBlocksRestartUntilResolved) {
  auto& state = inventory_hal_test::state;
  HalInventoryPathsStage stage;
  ASSERT_TRUE(stage.begin());
  state.failRemove = true;
  stage.abort();
  EXPECT_TRUE(state.files.contains(HalInventoryPathsStage::CANDIDATE));
  EXPECT_FALSE(stage.begin());
  state.failRemove = false;
  ASSERT_TRUE(stage.begin());
  state.failClose = true;
  stage.abort();
  EXPECT_TRUE(state.files.contains(HalInventoryPathsStage::CANDIDATE));
  state.failClose = false;
  stage.abort();
  EXPECT_FALSE(state.files.contains(HalInventoryPathsStage::CANDIDATE));
}
