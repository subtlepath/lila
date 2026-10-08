#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <iterator>
#include <vector>

#include "lib/Companion/CompanionDictionaryMembersValidation.h"

using namespace companion;
namespace {
class Source final : public DictionaryBundleSource {
 public:
  std::array<std::vector<uint8_t>, 4> members;
  unsigned reads = 0, failRead = 0, closes = 0, lastMember = 4;
  bool failClose = false;
  bool size(unsigned member, uint64_t& bytes) override {
    if (member >= members.size()) return false;
    bytes = members[member].size();
    return true;
  }
  bool read(unsigned member, uint64_t at, std::span<uint8_t> bytes) override {
    if (++reads == failRead || member >= members.size() || at > members[member].size() ||
        bytes.size() > members[member].size() - at)
      return false;
    std::copy_n(members[member].begin() + at, bytes.size(), bytes.begin());
    lastMember = member;
    return true;
  }
  bool close() override {
    ++closes;
    return !failClose;
  }
};
class CompanionDictionaryMembersValidation : public testing::Test {
 protected:
  Source source;
  std::array<uint8_t, 64> scratch;
  std::array<uint8_t, 32768> window;
  tinfl_decompressor decoder{};
  DictionaryMembersValidation validator{source, scratch, &decoder, window};
  static std::vector<uint8_t> fixture(const char* name) {
    std::ifstream file(std::string(COMPANION_FIXTURE_DIR) + "/" + name, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
  }
  void SetUp() override {
    source.members[0] = {'o', 'n', 'e', 't', 'w', 'o'};
    source.members[1] = fixture("DictionaryIndex-definitions.fixture");
    source.members[2] = fixture("DictionaryInfo.fixture");
    source.members[3] = fixture("DictionaryIndex-synonyms.fixture");
    ASSERT_EQ(source.members[1].size(), 24u);
    ASSERT_FALSE(source.members[2].empty());
  }
};
TEST_F(CompanionDictionaryMembersValidation, ValidatesPlainAndCompressedMembersTogether) {
  DictionaryMembersDetails result;
  ASSERT_TRUE(validator.validate(false, true, result));
  EXPECT_EQ(result.definitionBytes, 6u);
  EXPECT_EQ(result.info.words, 2u);
  EXPECT_EQ(result.info.synonyms, 1u);
  EXPECT_EQ(source.closes, 1u);
  source.members[0] = fixture("dictzip-chunks.dict.dz");
  ASSERT_TRUE(validator.validate(true, true, result));
  EXPECT_EQ(result.definitionBytes, 120000u);
  EXPECT_TRUE(result.compressed);
  EXPECT_EQ(source.closes, 2u);
}
TEST_F(CompanionDictionaryMembersValidation, PlainBundlesNeedNoDecoderOrHistoryWindow) {
  DictionaryMembersValidation plain(source, scratch);
  DictionaryMembersDetails result;
  ASSERT_TRUE(plain.validate(false, true, result));
  result.definitionBytes = 99;
  EXPECT_FALSE(plain.validate(true, true, result));
  EXPECT_EQ(result.definitionBytes, 99u);
}
TEST_F(CompanionDictionaryMembersValidation, RejectsCrossMemberSizeAndRangeConflicts) {
  const auto original = source.members;
  for (unsigned mutation = 0; mutation < 5; ++mutation) {
    source.members = original;
    if (mutation == 0) source.members[0].resize(5);
    if (mutation == 1) source.members[1].push_back(0);
    if (mutation == 2) source.members[3].back() = 2;
    if (mutation == 3) source.members[3].pop_back();
    if (mutation == 4) source.members[1][0] = 'z';
    DictionaryMembersDetails result;
    result.definitionBytes = 99;
    EXPECT_FALSE(validator.validate(false, true, result)) << mutation;
    EXPECT_EQ(result.definitionBytes, 99u);
  }
}
TEST_F(CompanionDictionaryMembersValidation, RequiresDeclaredNonemptySynonymsAndAllowsAbsentZeroCount) {
  DictionaryMembersDetails result;
  EXPECT_FALSE(validator.validate(false, false, result));
  std::string info(source.members[2].begin(), source.members[2].end());
  const auto at = info.find("synwordcount=1");
  ASSERT_NE(at, std::string::npos);
  info[at + 13] = '0';
  source.members[2].assign(info.begin(), info.end());
  EXPECT_TRUE(validator.validate(false, false, result));
  EXPECT_FALSE(validator.validate(false, true, result));
  info.erase(at, std::string("synwordcount=0\n").size());
  source.members[2].assign(info.begin(), info.end());
  EXPECT_TRUE(validator.validate(false, false, result));
  EXPECT_FALSE(validator.validate(false, true, result));
}
TEST_F(CompanionDictionaryMembersValidation, ReadCloseAndCancellationErrorsLeaveOutputUntouched) {
  DictionaryMembersDetails result;
  ASSERT_TRUE(validator.validate(false, true, result));
  const auto reads = source.reads;
  for (unsigned failure = 1; failure <= reads; ++failure) {
    source.reads = 0;
    source.failRead = failure;
    result.definitionBytes = 99;
    ASSERT_FALSE(validator.validate(false, true, result)) << failure;
    EXPECT_EQ(result.definitionBytes, 99u);
  }
  source.failRead = 0;
  source.failClose = true;
  EXPECT_FALSE(validator.validate(false, true, result));
  EXPECT_EQ(result.definitionBytes, 99u);
  source.failClose = false;
  DictionaryMembersValidation cancelled(source, scratch, &decoder, window, [](void*) { return false; });
  EXPECT_FALSE(cancelled.validate(false, true, result));
  EXPECT_EQ(result.definitionBytes, 99u);
  EXPECT_TRUE(validator.validate(false, true, result));
}
TEST_F(CompanionDictionaryMembersValidation, CancelsWhileStreamingIndexAndCanRetry) {
  DictionaryMembersValidation cancelled(
      source, std::span(scratch).first(12), nullptr, {},
      [](void* context) { return static_cast<Source*>(context)->lastMember != 1; }, &source);
  DictionaryMembersDetails result;
  result.definitionBytes = 99;
  EXPECT_FALSE(cancelled.validate(false, true, result));
  EXPECT_EQ(source.lastMember, 1u);
  EXPECT_EQ(result.definitionBytes, 99u);
  EXPECT_TRUE(validator.validate(false, true, result));
}
}  // namespace
