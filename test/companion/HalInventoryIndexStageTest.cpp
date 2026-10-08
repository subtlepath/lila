#include <gtest/gtest.h>

#include "../../lib/hal/HalInventoryIndexStage.h"
#include "../../lib/hal/HalInventoryPathsStage.h"
#include "../../lib/hal/HalInventoryPublicationValidator.h"
#include "../../lib/hal/HalTransferStorage.h"
using namespace companion;
namespace {
Identity generation() {
  Identity value{};
  value[0] = 1;
  return value;
}
ContentManifest item(unsigned hash) {
  ContentManifest value;
  value.contentHash[0] = hash;
  value.length = 99;
  return value;
}
struct Source : SortedInventorySource {
  unsigned cursor = 0, errorAt = 99;
  InventorySourceResult next(ContentManifest& manifest) override {
    if (cursor == errorAt) return InventorySourceResult::Error;
    if (cursor == 2) return InventorySourceResult::End;
    manifest = item(++cursor);
    return InventorySourceResult::Entry;
  }
};
class IndexStageTest : public testing::Test {
 protected:
  void SetUp() override { inventory_hal_test::state = {}; }
};
}  // namespace
TEST_F(IndexStageTest, SealsIndexWithOneWriterHandleAndPreservesAuthoritativeFile) {
  auto& state = inventory_hal_test::state;
  state.files[InventoryPublication::INDEX] = {42};
  std::array<uint8_t, 8192> scratch;
  {
    HalInventoryIndexStage stage;
    InventoryIndexBuilder builder(stage, scratch);
    Source source;
    ASSERT_TRUE(builder.build(source, generation(), 7));
    EXPECT_EQ(state.opens, 1u);
    EXPECT_EQ(state.closes, 1u);
    EXPECT_EQ(state.files.at(InventoryPublication::INDEX), std::vector<uint8_t>{42});
  }
  EXPECT_TRUE(state.files.contains(HalInventoryIndexStage::CANDIDATE));
  HalInventoryIndexStorage reader;
  ASSERT_TRUE(reader.open(HalInventoryIndexStage::CANDIDATE));
  IndexedInventoryCatalog catalog(reader, scratch);
  ASSERT_TRUE(catalog.open(generation()));
  EXPECT_EQ(catalog.revision(), 7u);
  EXPECT_EQ(catalog.count(), 2u);
}
TEST_F(IndexStageTest, ScanAndBackendFailuresDiscardCandidateWithoutChangingActive) {
  for (unsigned failure = 0; failure < 5; ++failure) {
    auto& state = inventory_hal_test::state;
    state = {};
    state.files[InventoryPublication::INDEX] = {42};
    std::array<uint8_t, 8192> scratch;
    HalInventoryIndexStage stage;
    InventoryIndexBuilder builder(stage, scratch);
    Source source;
    if (failure == 0) source.errorAt = 1;
    if (failure == 1) state.failOpen = true;
    if (failure == 2) state.failWrite = true;
    if (failure == 3) state.failSync = true;
    if (failure == 4) state.failClose = true;
    EXPECT_FALSE(builder.build(source, generation(), 7));
    EXPECT_EQ(state.files.at(InventoryPublication::INDEX), std::vector<uint8_t>{42});
    EXPECT_FALSE(state.files.contains(HalInventoryIndexStage::CANDIDATE));
  }
}
TEST_F(IndexStageTest, BuildsAndPublishesBothRealCandidatesThroughCoordinator) {
  std::array<uint8_t, 8192> scratch;
  HalInventoryIndexStage indexStage;
  InventoryIndexBuilder indexBuilder(indexStage, scratch);
  Source source;
  ASSERT_TRUE(indexBuilder.build(source, generation(), 7));
  HalInventoryPathsStage pathsStage;
  InventoryPathsBuilder pathsBuilder(pathsStage, scratch);
  ASSERT_TRUE(pathsBuilder.begin(generation(), 7));
  ASSERT_TRUE(pathsBuilder.record(item(1), "/books/one.epub"));
  ASSERT_TRUE(pathsBuilder.record(item(2), "/books/two.epub"));
  ASSERT_TRUE(pathsBuilder.seal());
  EXPECT_FALSE(inventory_hal_test::state.files.contains(InventoryPublication::INDEX));
  EXPECT_FALSE(inventory_hal_test::state.files.contains(InventoryPublication::PATHS));
  HalTransferStorage storage;
  HalInventoryPublicationValidator validator(scratch);
  InventoryPublication publication(storage, validator, scratch);
  ASSERT_EQ(publication.publish(generation(), 7), InventoryPublicationResult::Ok);
  ASSERT_EQ(validator.pair(InventoryPublication::INDEX, InventoryPublication::PATHS, generation(), 7),
            InventoryValidation::Valid);
  EXPECT_FALSE(inventory_hal_test::state.files.contains(HalInventoryIndexStage::CANDIDATE));
  EXPECT_FALSE(inventory_hal_test::state.files.contains(HalInventoryPathsStage::CANDIDATE));
}
