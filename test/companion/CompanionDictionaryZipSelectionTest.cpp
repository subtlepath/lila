#include <gtest/gtest.h>

#include <array>
#include <string>

#include "lib/Companion/CompanionDictionaryZipSelection.h"
using namespace companion;
namespace {
bool add(DictionaryZipSelection& select, const std::string& path, unsigned offset, bool directory = false,
         uint64_t size = 3) {
  ZipEntryMetadata entry;
  entry.payload = {offset, size, size, 0, 0, 0};
  entry.directory = directory;
  return select.add(std::span(reinterpret_cast<const uint8_t*>(path.data()), path.size()), entry);
}
TEST(DictionaryZipSelectionTest, EveryMemberOrderAndPlainPreferenceSelectSameSpans) {
  std::array<unsigned, 5> order{0, 1, 2, 3, 4};
  const std::array<std::string, 5> names{"folder/café.dict", "folder/café.idx", "folder/café.ifo", "folder/café.syn",
                                         "folder/café.dict.dz"};
  do {
    DictionaryZipSelection select;
    select.begin();
    for (auto at : order) ASSERT_TRUE(add(select, names[at], at + 1));
    ASSERT_TRUE(select.finishHeaders());
    for (auto at : order) ASSERT_TRUE(add(select, names[at], at + 1));
    DictionaryZipMembers output;
    ASSERT_TRUE(select.finishMembers(output));
    EXPECT_EQ(output.definitions.offset, 1u);
    EXPECT_EQ(output.index.offset, 2u);
    EXPECT_EQ(output.info.offset, 3u);
    EXPECT_FALSE(output.compressed);
    EXPECT_TRUE(output.hasSynonyms);
  } while (std::next_permutation(order.begin(), order.end()));
}
TEST(DictionaryZipSelectionTest, CompressedFallbackAndMissingOptionalSynonyms) {
  DictionaryZipSelection select;
  select.begin();
  ASSERT_TRUE(add(select, "demo.ifo", 1));
  ASSERT_TRUE(select.finishHeaders());
  ASSERT_TRUE(add(select, "demo.ifo", 1));
  ASSERT_TRUE(add(select, "demo.idx", 2));
  ASSERT_TRUE(add(select, "demo.dict", 3, true, 0));
  ASSERT_TRUE(add(select, "demo.dict.dz", 4));
  DictionaryZipMembers out;
  ASSERT_TRUE(select.finishMembers(out));
  EXPECT_TRUE(out.compressed);
  EXPECT_FALSE(out.hasSynonyms);
  EXPECT_EQ(out.definitions.offset, 4u);
}
TEST(DictionaryZipSelectionTest, InvalidHeadersMissingMembersAndChangedHeaderFailClosed) {
  DictionaryZipSelection select;
  select.begin();
  EXPECT_FALSE(select.finishHeaders());
  select.begin();
  EXPECT_FALSE(add(select, "demo.ifo", 1, true));
  select.begin();
  EXPECT_FALSE(add(select, "demo.ifo", 1, false, 65537));
  select.begin();
  ASSERT_TRUE(add(select, "a.ifo", 1));
  EXPECT_FALSE(add(select, "b.ifo", 2));
  select.begin();
  ASSERT_TRUE(add(select, "a.ifo", 1));
  ASSERT_TRUE(select.finishHeaders());
  EXPECT_FALSE(add(select, "a.ifo", 2));
  select.begin();
  ASSERT_TRUE(add(select, "a.ifo", 1));
  ASSERT_TRUE(select.finishHeaders());
  ASSERT_TRUE(add(select, "a.ifo", 1));
  DictionaryZipMembers out;
  out.info.offset = 99;
  EXPECT_FALSE(select.finishMembers(out));
  EXPECT_EQ(out.info.offset, 99u);
}
TEST(DictionaryZipSelectionTest, ReuseClearsOptionalMemberFromPreviousArchive) {
  DictionaryZipSelection select;
  DictionaryZipMembers out;
  for (bool synonyms : {true, false}) {
    select.begin();
    ASSERT_TRUE(add(select, "demo.ifo", 1));
    ASSERT_TRUE(select.finishHeaders());
    ASSERT_TRUE(add(select, "demo.ifo", 1));
    ASSERT_TRUE(add(select, "demo.idx", 2));
    ASSERT_TRUE(add(select, "demo.dict", 3));
    if (synonyms) ASSERT_TRUE(add(select, "demo.syn", 4));
    ASSERT_TRUE(select.finishMembers(out));
    EXPECT_EQ(out.hasSynonyms, synonyms);
    EXPECT_EQ(out.synonyms.offset, synonyms ? 4u : 0u);
    EXPECT_EQ(out.synonyms.expandedBytes, synonyms ? 3u : 0u);
  }
}
}  // namespace
