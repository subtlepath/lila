#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <iterator>

#include "lib/Companion/CompanionZipArchiveValidation.h"
#include "lib/hal/HalInventoryFileView.h"
#include "lib/hal/HalZipRangeStorage.h"
using namespace companion;
namespace {
class HalZipRangeTest : public testing::Test {
 protected:
  void SetUp() override { inventory_hal_test::state = {}; }
};
TEST_F(HalZipRangeTest, SortReusesHandlesYieldsAndDiscardsOnlyOwnedStage) {
  HalZipRangeStorage storage;
  ZipRangeValidation ranges(storage);
  ASSERT_TRUE(ranges.begin(10000));
  for (uint64_t i = 100; i-- > 0;) ASSERT_TRUE(ranges.add(i * 20, i * 20 + 20));
  ASSERT_TRUE(ranges.finish());
  EXPECT_EQ(inventory_hal_test::state.files.at(HalZipRangeStorage::PATH).size(), 1600u);
  ASSERT_TRUE(storage.discard());
  EXPECT_FALSE(inventory_hal_test::state.files.contains(HalZipRangeStorage::PATH));
  ASSERT_TRUE(ranges.begin(100));
  ASSERT_TRUE(ranges.add(0, 10));
  ASSERT_TRUE(ranges.finish());
  EXPECT_EQ(inventory_hal_test::state.preparations, 3u);
  EXPECT_GT(inventory_hal_test::state.yields, 0u);
}
TEST_F(HalZipRangeTest, UnknownFileAndDirectoryArePreserved) {
  const std::vector<uint8_t> original{1, 2, 3};
  inventory_hal_test::state.files[HalZipRangeStorage::PATH] = original;
  {
    HalZipRangeStorage storage;
    EXPECT_FALSE(storage.reset());
  }
  EXPECT_EQ(inventory_hal_test::state.files.at(HalZipRangeStorage::PATH), original);
  inventory_hal_test::state.files.clear();
  inventory_hal_test::state.directories[HalZipRangeStorage::PATH] = {};
  {
    HalZipRangeStorage storage;
    EXPECT_FALSE(storage.reset());
  }
  EXPECT_TRUE(inventory_hal_test::state.directories.contains(HalZipRangeStorage::PATH));
}
TEST_F(HalZipRangeTest, WriteCorruptionAndSealFailuresFailClosed) {
  for (unsigned fault = 0; fault < 5; ++fault) {
    inventory_hal_test::state = {};
    HalZipRangeStorage storage;
    ZipRangeValidation ranges(storage);
    ASSERT_TRUE(ranges.begin(100));
    auto& state = inventory_hal_test::state;
    if (fault == 0) state.corruptWrite = true;
    if (fault == 1) state.failWrite = true;
    if (fault < 2)
      EXPECT_FALSE(ranges.add(0, 10));
    else {
      ASSERT_TRUE(ranges.add(0, 10));
      if (fault == 2) state.failSync = true;
      if (fault == 3) state.failTruncate = true;
      if (fault == 4) state.failClose = true;
      EXPECT_FALSE(ranges.finish());
    }
    state.corruptWrite = state.failWrite = state.failSync = state.failTruncate = state.failClose = false;
    EXPECT_TRUE(storage.discard());
  }
}
TEST_F(HalZipRangeTest, CleanupFailureRetainsOwnershipForRetry) {
  HalZipRangeStorage storage;
  ASSERT_TRUE(storage.reset());
  inventory_hal_test::state.failRemove = true;
  EXPECT_FALSE(storage.discard());
  EXPECT_TRUE(inventory_hal_test::state.files.contains(HalZipRangeStorage::PATH));
  inventory_hal_test::state.failRemove = false;
  EXPECT_TRUE(storage.discard());
  EXPECT_FALSE(inventory_hal_test::state.files.contains(HalZipRangeStorage::PATH));
}
TEST_F(HalZipRangeTest, CompleteArchiveSweepUsesBorrowedHalSourceAndRangeStage) {
  std::array<uint8_t, 64> scratch{};
  tinfl_decompressor decoder{};
  std::array<uint8_t, 32768> window{};
  for (const char* name : {"DictionaryBundle-plain.fixture", "ZipEntry-zip64-streamed.fixture"}) {
    std::ifstream fixture(std::string(ZIP_HAL_RANGE_DIR) + "/" + name, std::ios::binary);
    const std::vector<uint8_t> original{std::istreambuf_iterator<char>(fixture), {}};
    ASSERT_FALSE(original.empty());
    inventory_hal_test::state.files["/incoming.zip"] = original;
    HalFile file;
    ASSERT_TRUE(Storage.openFileForReadReusing("TEST", "/incoming.zip", file));
    HalInventoryFileView source;
    ASSERT_TRUE(source.attach(file));
    HalZipRangeStorage storage;
    ZipRangeValidation ranges(storage);
    ZipArchiveValidation parser(source, scratch, &decoder, window);
    ZipDirectoryLayout output;
    ASSERT_TRUE(parser.validate(output, &ranges)) << name;
    EXPECT_EQ(output.archiveBytes, original.size());
    EXPECT_EQ(inventory_hal_test::state.files.at(HalZipRangeStorage::PATH).size(), output.entries * 16);
    ASSERT_TRUE(storage.discard());
    inventory_hal_test::state.corruptWrite = true;
    output.entries = 99;
    EXPECT_FALSE(parser.validate(output, &ranges));
    EXPECT_EQ(output.entries, 99u);
    EXPECT_FALSE(ranges.finish());
    inventory_hal_test::state.corruptWrite = false;
    ASSERT_TRUE(storage.discard());
    ASSERT_TRUE(parser.validate(output, &ranges));
    EXPECT_EQ(inventory_hal_test::state.files.at("/incoming.zip"), original);
  }
}

TEST_F(HalZipRangeTest, RangeAndNameIndexStagesHaveIndependentOwnershipAndCleanup) {
  HalZipRangeStorage ranges;
  HalZipRangeStorage names(HalZipRangeStorage::Purpose::NameIndex);
  ASSERT_TRUE(ranges.reset());
  ASSERT_TRUE(names.reset());
  std::array<uint8_t, 16> record{};
  record[0] = 1;
  ASSERT_TRUE(ranges.write(0, record));
  record[0] = 2;
  ASSERT_TRUE(names.write(0, record));
  ASSERT_TRUE(ranges.seal(16));
  ASSERT_TRUE(names.seal(16));
  EXPECT_EQ(inventory_hal_test::state.files.at(HalZipRangeStorage::PATH)[0], 1);
  EXPECT_EQ(inventory_hal_test::state.files.at(HalZipRangeStorage::NAME_INDEX_PATH)[0], 2);
  ASSERT_TRUE(names.discard());
  EXPECT_TRUE(inventory_hal_test::state.files.contains(HalZipRangeStorage::PATH));
  EXPECT_FALSE(inventory_hal_test::state.files.contains(HalZipRangeStorage::NAME_INDEX_PATH));
  ASSERT_TRUE(ranges.discard());
}
TEST_F(HalZipRangeTest, PreexistingNameIndexCannotBeClaimedOrRemoved) {
  const std::vector<uint8_t> original{3, 4, 5};
  inventory_hal_test::state.files[HalZipRangeStorage::NAME_INDEX_PATH] = original;
  {
    HalZipRangeStorage names(HalZipRangeStorage::Purpose::NameIndex);
    EXPECT_FALSE(names.reset());
  }
  EXPECT_EQ(inventory_hal_test::state.files.at(HalZipRangeStorage::NAME_INDEX_PATH), original);
  {
    HalZipRangeStorage ranges;
    ASSERT_TRUE(ranges.reset());
  }
  EXPECT_EQ(inventory_hal_test::state.files.at(HalZipRangeStorage::NAME_INDEX_PATH), original);
}

}  // namespace
