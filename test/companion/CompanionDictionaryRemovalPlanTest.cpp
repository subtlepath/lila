#include <gtest/gtest.h>

#include <cstring>

#include "lib/Companion/CompanionDictionaryRemovalPlan.h"
using namespace companion;
namespace {
DictionaryRemovalPlan plan(bool compressed = false, bool synonyms = false) {
  DictionaryRemovalPlan result;
  result.request.transaction.fill(1);
  result.request.owner.fill(2);
  result.request.generation.fill(3);
  result.request.manifest.kind = ContentKind::Dictionary;
  result.request.manifest.formatVersion = 1;
  result.request.manifest.length = 1000;
  result.request.manifest.contentHash.fill(4);
  auto& installed = result.installed;
  installed.revision = 1;
  installed.phase = DictionaryInstallationPhase::Committed;
  installed.extraction.revision = 1;
  installed.extraction.transaction = result.request.transaction;
  installed.extraction.generation = result.request.generation;
  installed.extraction.archiveHash = result.request.manifest.contentHash;
  installed.extraction.compressed = compressed;
  installed.extraction.synonyms = synonyms;
  installed.extraction.sealed = installed.published = synonyms ? 15 : 7;
  installed.extraction.lengths = {6, 24, 111, synonyms ? 12U : 0U};
  for (unsigned at = 0; at < (synonyms ? 4U : 3U); ++at) installed.extraction.hashes[at].fill(at + 5);
  installed.archives.original = result.request.manifest;
  installed.archives.members = result.request.manifest;
  installed.archives.members.contentHash.fill(9);
  std::strcpy(installed.base.data(), "/dictionaries/Family/dictionary");
  return result;
}
}  // namespace
TEST(DictionaryRemovalPlan, RoundtripRetainsRequestArchiveAssociationAndMemberProofs) {
  for (bool compressed : {false, true})
    for (bool synonyms : {false, true}) {
      const auto input = plan(compressed, synonyms);
      ASSERT_TRUE(validDictionaryRemovalPlan(input));
      std::array<uint8_t, DICTIONARY_REMOVAL_PLAN_SIZE + 2> bytes{};
      bytes.front() = 0xaa;
      bytes.back() = 0xbb;
      auto encoded = std::span(bytes).subspan(1, DICTIONARY_REMOVAL_PLAN_SIZE);
      ASSERT_EQ(DictionaryRemovalPlanCodec::encode(input, encoded), encoded.size());
      DictionaryRemovalPlanCodec codec;
      auto output = plan(!compressed, !synonyms);
      ASSERT_TRUE(codec.decode(encoded, output));
      EXPECT_EQ(output, input);
      EXPECT_EQ(bytes.front(), 0xaa);
      EXPECT_EQ(bytes.back(), 0xbb);
      for (size_t length = 0; length < encoded.size(); ++length) {
        EXPECT_FALSE(codec.decode(encoded.first(length), output));
        EXPECT_EQ(output, input);
      }
      for (unsigned member = 0; member < 4; ++member) {
        std::array<char, DICTIONARY_INSTALLATION_MEMBER_PATH_CAPACITY> path{};
        if (member == 3 && !synonyms) {
          EXPECT_FALSE(dictionaryRemovalMemberPath(input, member, path));
          continue;
        }
        ASSERT_TRUE(dictionaryRemovalMemberPath(input, member, path));
        static constexpr const char* suffix[] = {".dict", ".idx", ".ifo", ".syn"};
        EXPECT_EQ(std::string(path.data()),
                  std::string(input.installed.base.data()) + (member == 0 && compressed ? ".dict.dz" : suffix[member]));
      }
    }
}
TEST(DictionaryRemovalPlan, CorruptionAndInvalidOwnershipPreserveCallerOutput) {
  const auto input = plan(true, true);
  std::array<uint8_t, DICTIONARY_REMOVAL_PLAN_SIZE> bytes{};
  ASSERT_EQ(DictionaryRemovalPlanCodec::encode(input, bytes), bytes.size());
  DictionaryRemovalPlanCodec codec;
  auto output = plan();
  const auto unchanged = output;
  for (size_t at = 0; at < bytes.size(); ++at) {
    auto invalid = bytes;
    invalid[at] ^= 1;
    EXPECT_FALSE(codec.decode(invalid, output));
    EXPECT_EQ(output, unchanged);
  }
  for (unsigned fault = 0; fault < 7; ++fault) {
    auto invalid = input;
    switch (fault) {
      case 0:
        invalid.installed.extraction.transaction.fill(7);
        break;
      case 1:
        invalid.installed.extraction.generation.fill(7);
        break;
      case 2:
        invalid.request.manifest.contentHash.fill(7);
        break;
      case 3:
        invalid.installed.phase = DictionaryInstallationPhase::Bound;
        break;
      case 4:
        invalid.installed.published = 7;
        break;
      case 5:
        std::strcpy(invalid.installed.base.data(), "/.crosspoint/dictionary/cache");
        break;
      case 6:
        invalid.request.manifest.kind = ContentKind::Epub;
        break;
    }
    EXPECT_FALSE(validDictionaryRemovalPlan(invalid));
    auto untouched = bytes;
    EXPECT_EQ(DictionaryRemovalPlanCodec::encode(invalid, untouched), 0u);
    EXPECT_EQ(untouched, bytes);
  }
  // Keep the checksum valid so decoding must reject mismatched ownership.
  bytes[8 + 4] ^= 1;
  inventory_detail::write(bytes, bytes.size() - 4, inventoryIndexCrc(std::span(bytes).first(bytes.size() - 4)), 4);
  EXPECT_FALSE(codec.decode(bytes, output));
  EXPECT_EQ(output, unchanged);
}
TEST(DictionaryRemovalPlan, HiddenRootAndExactMemberBufferBoundary) {
  auto input = plan(true, true);
  std::strcpy(input.installed.base.data(), "/.dictionaries/Family/dictionary");
  ASSERT_TRUE(validDictionaryRemovalPlan(input));
  const size_t required = std::strlen(input.installed.base.data()) + 8;
  std::array<char, DICTIONARY_INSTALLATION_MEMBER_PATH_CAPACITY> bytes{};
  bytes.fill('x');
  const auto unchanged = bytes;
  EXPECT_FALSE(dictionaryRemovalMemberPath(input, 0, std::span(bytes).first(required)));
  EXPECT_EQ(bytes, unchanged);
  ASSERT_TRUE(dictionaryRemovalMemberPath(input, 0, std::span(bytes).first(required + 1)));
  EXPECT_EQ(bytes[required], 0);
  EXPECT_EQ(bytes[required + 1], 'x');
  EXPECT_FALSE(dictionaryRemovalMemberPath(input, 4, bytes));
}
