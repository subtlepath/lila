#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <iterator>
#include <vector>

#include "lib/Companion/CompanionDictionaryMembersValidation.h"
#include "lib/hal/HalDictionaryBundleSource.h"

using namespace companion;
namespace {
class Stage final : public DictionaryBundleStage {
 public:
  std::vector<uint8_t> bytes;
  bool sealed = false;
  bool begin() override {
    bytes.clear();
    return true;
  }
  bool write(uint64_t at, std::span<const uint8_t> input) override {
    if (at != bytes.size()) return false;
    bytes.insert(bytes.end(), input.begin(), input.end());
    return true;
  }
  bool seal(uint64_t length) override {
    sealed = length == bytes.size();
    return sealed;
  }
  void abort() override { bytes.clear(); }
};
class HalDictionaryBundleSourceTest : public testing::Test {
 protected:
  HalDictionaryBundleSource source;
  std::array<uint8_t, 64> scratch;
  std::array<uint8_t, 32768> window;
  tinfl_decompressor decoder{};
  static constexpr const char* BASE = "/dictionaries/es/stem";
  static std::vector<uint8_t> fixture(const char* name) {
    std::ifstream file(std::string(COMPANION_FIXTURE_DIR) + "/" + name, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
  }
  void SetUp() override {
    inventory_hal_test::state = {};
    auto& files = inventory_hal_test::state.files;
    files[std::string(BASE) + ".dict"] = {'o', 'n', 'e', 't', 'w', 'o'};
    files[std::string(BASE) + ".idx"] = fixture("DictionaryIndex-definitions.fixture");
    files[std::string(BASE) + ".ifo"] = fixture("DictionaryInfo.fixture");
    files[std::string(BASE) + ".syn"] = fixture("DictionaryIndex-synonyms.fixture");
    files[std::string(BASE) + ".dict.dz"] = fixture("dictzip-chunks.dict.dz");
  }
};
TEST_F(HalDictionaryBundleSourceTest, ValidatesReopensAndBuildsWithFourRetainedHandles) {
  ASSERT_TRUE(source.begin(BASE, false, true));
  DictionaryMembersValidation validation(source, scratch);
  DictionaryMembersDetails details;
  ASSERT_TRUE(validation.validate(false, true, details));
  EXPECT_EQ(inventory_hal_test::state.preparations, 4u);
  ASSERT_TRUE(source.reopen());
  Stage stage;
  DictionaryBundleBuilder builder(stage, scratch);
  uint64_t length = 0;
  ASSERT_TRUE(builder.build(source, false, true, length));
  EXPECT_TRUE(stage.sealed);
  EXPECT_EQ(length, stage.bytes.size());
  EXPECT_GT(length, 6u);
  EXPECT_EQ(inventory_hal_test::state.preparations, 4u);
  EXPECT_EQ(inventory_hal_test::state.closes, 8u);
  ASSERT_TRUE(source.begin(BASE, true, true));
  DictionaryMembersValidation compressed(source, scratch, &decoder, window);
  ASSERT_TRUE(compressed.validate(true, true, details));
  EXPECT_EQ(details.definitionBytes, 120000u);
  EXPECT_EQ(inventory_hal_test::state.preparations, 4u);
}
TEST_F(HalDictionaryBundleSourceTest, RejectsMissingDirectoryAndInvalidPathsAndCanRetry) {
  auto& state = inventory_hal_test::state;
  state.files.erase(std::string(BASE) + ".idx");
  EXPECT_FALSE(source.begin(BASE, false, true));
  EXPECT_FALSE(source.reopen());
  state.directories[std::string(BASE) + ".idx"] = {};
  EXPECT_FALSE(source.begin(BASE, false, true));
  state.directories.clear();
  state.files[std::string(BASE) + ".idx"] = fixture("DictionaryIndex-definitions.fixture");
  EXPECT_FALSE(source.begin("/dictionaries/../stem", false, true));
  EXPECT_FALSE(source.begin(std::string(504, 'a'), false, true));
  EXPECT_TRUE(source.begin(BASE, false, true));
}
TEST_F(HalDictionaryBundleSourceTest, BoundsReadAndIoErrorsAreStickyUntilBegin) {
  ASSERT_TRUE(source.begin(BASE, false, true));
  std::array<uint8_t, 2> bytes{};
  EXPECT_FALSE(source.read(0, 5, bytes));
  uint64_t length = 99;
  EXPECT_FALSE(source.size(0, length));
  EXPECT_EQ(length, 99u);
  ASSERT_TRUE(source.begin(BASE, false, true));
  inventory_hal_test::state.shortRead = inventory_hal_test::state.reads + 1;
  EXPECT_FALSE(source.read(0, 0, bytes));
  EXPECT_FALSE(source.size(0, length));
  inventory_hal_test::state.shortRead = 0;
  ASSERT_TRUE(source.begin(BASE, false, true));
  EXPECT_TRUE(source.read(0, 0, bytes));
  EXPECT_EQ(bytes[0], 'o');
}
TEST_F(HalDictionaryBundleSourceTest, LengthChangeReopenAndCloseFailurePreserveFailure) {
  ASSERT_TRUE(source.begin(BASE, false, true));
  ASSERT_TRUE(source.close());
  inventory_hal_test::state.files[std::string(BASE) + ".dict"].push_back('x');
  EXPECT_FALSE(source.reopen());
  EXPECT_FALSE(source.reopen());
  ASSERT_TRUE(source.begin(BASE, false, true));
  inventory_hal_test::state.failClose = true;
  EXPECT_FALSE(source.close());
  uint64_t length = 99;
  EXPECT_FALSE(source.size(0, length));
  EXPECT_EQ(length, 99u);
  inventory_hal_test::state.failClose = false;
  EXPECT_TRUE(source.begin(BASE, false, true));
}
TEST_F(HalDictionaryBundleSourceTest, YieldsEveryThirtyTwoReadsAndRetainsAllocationsAcrossScans) {
  ASSERT_TRUE(source.begin(BASE, false, true));
  uint8_t byte;
  for (unsigned read = 0; read < 64; ++read) ASSERT_TRUE(source.read(0, 0, std::span(&byte, 1)));
  EXPECT_EQ(inventory_hal_test::state.yields, 2u);
  for (unsigned scan = 0; scan < 10; ++scan) ASSERT_TRUE(source.begin(BASE, false, true));
  EXPECT_EQ(inventory_hal_test::state.preparations, 4u);
}
}  // namespace
