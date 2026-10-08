#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <iterator>
#include <vector>

#include "lib/Companion/CompanionDictionaryMembersValidation.h"
#include "lib/hal/HalDictionaryBundleSource.h"
#include "lib/hal/HalDictionaryDiscovery.h"

using namespace companion;
namespace {
class HalDictionaryDiscoveryTest : public testing::Test {
 protected:
  HalDictionaryDiscovery discovery;
  static constexpr const char* FOLDER = "/dictionaries/es";
  void SetUp() override {
    inventory_hal_test::state = {};
    inventory_hal_test::state.directories[FOLDER] = {{"stem.ifo"},     {"readme.txt"}, {"._stem.idx"}, {"stem.dict.dz"},
                                                     {"nested", true}, {"stem.syn"},   {"stem.idx"}};
  }
  static std::vector<uint8_t> fixture(const char* name) {
    std::ifstream file(std::string(COMPANION_FIXTURE_DIR) + "/" + name, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
  }
};
TEST_F(HalDictionaryDiscoveryTest, FindsOneStemAndPrefersPlainDataWhenBothExist) {
  DictionaryDiscoveryDetails details;
  ASSERT_EQ(discovery.inspect(FOLDER, details), DictionaryDiscoveryResult::Found);
  EXPECT_STREQ(discovery.basePath(), "/dictionaries/es/stem");
  EXPECT_TRUE(details.compressed);
  EXPECT_TRUE(details.synonyms);
  const std::string_view borrowedFolder(discovery.basePath(), std::strlen(FOLDER));
  ASSERT_EQ(discovery.inspect(borrowedFolder, details), DictionaryDiscoveryResult::Found);
  inventory_hal_test::state.directories[FOLDER].push_back({"stem.dict"});
  ASSERT_EQ(discovery.inspect(FOLDER, details), DictionaryDiscoveryResult::Found);
  EXPECT_FALSE(details.compressed);
  EXPECT_EQ(inventory_hal_test::state.preparations, 2u);
}
TEST_F(HalDictionaryDiscoveryTest, DistinguishesEmptyFoldersFromIncompleteOrAmbiguousBundles) {
  auto& entries = inventory_hal_test::state.directories[FOLDER];
  const auto original = entries;
  DictionaryDiscoveryDetails details{true, true};
  entries = {{"readme.txt"}, {"subdir", true}, {"._resource.idx"}};
  EXPECT_EQ(discovery.inspect(FOLDER, details), DictionaryDiscoveryResult::Empty);
  EXPECT_STREQ(discovery.basePath(), "");
  EXPECT_EQ(details, (DictionaryDiscoveryDetails{true, true}));
  entries = {{"stem.idx"}, {"stem.ifo"}};
  EXPECT_EQ(discovery.inspect(FOLDER, details), DictionaryDiscoveryResult::Error);
  entries = original;
  entries.push_back({"other.idx"});
  EXPECT_EQ(discovery.inspect(FOLDER, details), DictionaryDiscoveryResult::Error);
  entries = original;
  entries.push_back({"stem.ifo"});
  EXPECT_EQ(discovery.inspect(FOLDER, details), DictionaryDiscoveryResult::Error);
}
TEST_F(HalDictionaryDiscoveryTest, RejectsUnsafeTruncatedNamesAndReadOrCloseErrors) {
  auto& state = inventory_hal_test::state;
  const auto original = state.directories[FOLDER];
  DictionaryDiscoveryDetails details{true, true};
  for (const auto& name : {std::string("bad:name"), std::string(256, 'a'), std::string("bad\\name")}) {
    state.directories[FOLDER] = {{name}};
    EXPECT_EQ(discovery.inspect(FOLDER, details), DictionaryDiscoveryResult::Error);
    EXPECT_STREQ(discovery.basePath(), "");
    EXPECT_EQ(details, (DictionaryDiscoveryDetails{true, true}));
  }
  state.directories[FOLDER] = original;
  state.directoryErrorPath = FOLDER;
  EXPECT_EQ(discovery.inspect(FOLDER, details), DictionaryDiscoveryResult::Error);
  state.directoryErrorPath.clear();
  state.failClose = true;
  EXPECT_EQ(discovery.inspect(FOLDER, details), DictionaryDiscoveryResult::Error);
  state.failClose = false;
  EXPECT_EQ(discovery.inspect("/dictionaries/../es", details), DictionaryDiscoveryResult::Error);
  EXPECT_EQ(discovery.inspect(FOLDER, details), DictionaryDiscoveryResult::Found);
}
TEST_F(HalDictionaryDiscoveryTest, RetainsHandlesAndYieldsDuringLargeFolderScans) {
  auto& state = inventory_hal_test::state;
  auto& entries = state.directories[FOLDER];
  entries.reserve(107);
  for (unsigned index = 0; index < 100; ++index) entries.push_back({"readme" + std::to_string(index)});
  DictionaryDiscoveryDetails details;
  for (unsigned scan = 0; scan < 5; ++scan)
    ASSERT_EQ(discovery.inspect(FOLDER, details), DictionaryDiscoveryResult::Found);
  EXPECT_EQ(state.preparations, 2u);
  EXPECT_GT(state.yields, 20u);
}
TEST_F(HalDictionaryDiscoveryTest, DiscoveredCompressedMembersPassCompleteValidation) {
  auto& files = inventory_hal_test::state.files;
  files["/dictionaries/es/stem.dict.dz"] = fixture("dictzip-chunks.dict.dz");
  files["/dictionaries/es/stem.idx"] = fixture("DictionaryIndex-definitions.fixture");
  files["/dictionaries/es/stem.ifo"] = fixture("DictionaryInfo.fixture");
  files["/dictionaries/es/stem.syn"] = fixture("DictionaryIndex-synonyms.fixture");
  DictionaryDiscoveryDetails discovered;
  ASSERT_EQ(discovery.inspect(FOLDER, discovered), DictionaryDiscoveryResult::Found);
  HalDictionaryBundleSource source;
  ASSERT_TRUE(source.begin(discovery.basePath(), discovered.compressed, discovered.synonyms));
  std::array<uint8_t, 64> scratch;
  std::array<uint8_t, 32768> window;
  tinfl_decompressor decoder{};
  DictionaryMembersValidation validation(source, scratch, &decoder, window);
  DictionaryMembersDetails validated;
  ASSERT_TRUE(validation.validate(discovered.compressed, discovered.synonyms, validated));
  EXPECT_EQ(validated.definitionBytes, 120000u);
  EXPECT_EQ(validated.info.words, 2u);
}
TEST_F(HalDictionaryDiscoveryTest, CancellationDoesNotExposePartialDiscoveryAndCanRetry) {
  unsigned calls = 0;
  HalDictionaryDiscovery cancelled([](void* context) { return ++*static_cast<unsigned*>(context) < 4; }, &calls);
  DictionaryDiscoveryDetails details{false, true};
  EXPECT_EQ(cancelled.inspect(FOLDER, details), DictionaryDiscoveryResult::Error);
  EXPECT_STREQ(cancelled.basePath(), "");
  EXPECT_EQ(details, (DictionaryDiscoveryDetails{false, true}));
  calls = 0;
  inventory_hal_test::state.directories[FOLDER] = {};
  EXPECT_EQ(cancelled.inspect(FOLDER, details), DictionaryDiscoveryResult::Empty);
}
}  // namespace
