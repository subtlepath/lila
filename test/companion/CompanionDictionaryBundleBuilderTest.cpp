#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <iterator>
#include <tuple>
#include <vector>

#include "lib/Companion/CompanionDictionaryBundleBuilder.h"
using namespace companion;
namespace {
std::vector<uint8_t> fixture(const char* name) {
  std::ifstream file(std::string(BUNDLE_FIXTURE_DIR) + name, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
class Source final : public DictionaryBundleSource {
 public:
  std::array<std::vector<uint8_t>, 4> members;
  unsigned reads = 0, failRead = 0, closes = 0;
  bool failSize = false, failClose = false, oversized = false, changed = false;
  void prepare(bool compressed, bool synonyms) {
    members[0] = compressed ? fixture("dictzip-chunks.dict.dz") : std::vector<uint8_t>{'o', 'n', 'e', 't', 'w', 'o'};
    members[1] = {'o', 'n', 'e', 0, 0, 0, 0, 0, 0, 0, 0, 3, 't', 'w', 'o', 0, 0, 0, 0, 3, 0, 0, 0, 3};
    const std::string info =
        "StarDict's dict ifo file\nversion=3.0.0\nbookname=Canonical dictionary\nwordcount=2\nidxfilesize=24\n" +
        std::string(synonyms ? "synwordcount=1\n" : "");
    members[2] = {info.begin(), info.end()};
    members[3] = {'a', 'l', 'i', 'a', 's', 0, 0, 0, 0, 0};
  }
  bool size(unsigned index, uint64_t& bytes) override {
    if (failSize) return false;
    bytes = oversized ? uint64_t{UINT32_MAX} : members.at(index).size() + (changed && reads ? 1 : 0);
    return true;
  }
  bool read(unsigned index, uint64_t offset, std::span<uint8_t> bytes) override {
    if (++reads == failRead || offset > members.at(index).size() || bytes.size() > members.at(index).size() - offset)
      return false;
    std::copy_n(members[index].begin() + offset, bytes.size(), bytes.begin());
    return true;
  }
  bool close() override {
    ++closes;
    return !failClose;
  }
};
class Stage final : public DictionaryBundleStage {
 public:
  std::vector<uint8_t> bytes;
  unsigned writes = 0, failWrite = 0, begins = 0, aborts = 0;
  bool failBegin = false, failSeal = false, sealed = false;
  Stage() { bytes.reserve(8192); }
  bool begin() override {
    ++begins;
    bytes.clear();
    sealed = false;
    writes = 0;
    return !failBegin;
  }
  bool write(uint64_t offset, std::span<const uint8_t> chunk) override {
    if (++writes == failWrite || offset != bytes.size()) return false;
    bytes.insert(bytes.end(), chunk.begin(), chunk.end());
    return true;
  }
  bool seal(uint64_t length) override {
    sealed = !failSeal && length == bytes.size();
    return sealed;
  }
  void abort() override {
    ++aborts;
    bytes.clear();
    sealed = false;
  }
};
class CompanionDictionaryBundleBuilder : public testing::Test {
 protected:
  Source source;
  Stage stage;
  std::array<uint8_t, 512> scratch;
  DictionaryBundleBuilder builder{stage, std::span(scratch).first(64)};
  void SetUp() override { source.prepare(false, true); }
};
}  // namespace
TEST_F(CompanionDictionaryBundleBuilder, MatchesSharedArchivesIndependentOfChunkSize) {
  for (const auto& config : {std::tuple{false, true, "DictionaryBundle-plain.fixture"},
                             std::tuple{true, true, "DictionaryBundle-dictzip.fixture"},
                             std::tuple{false, false, "DictionaryBundle-no-syn.fixture"}}) {
    const auto [compressed, synonyms, name] = config;
    source.prepare(compressed, synonyms);
    uint64_t length = 42;
    ASSERT_TRUE(builder.build(source, compressed, synonyms, length));
    EXPECT_EQ(stage.bytes, fixture(name));
    EXPECT_EQ(length, stage.bytes.size());
    EXPECT_TRUE(stage.sealed);
    DictionaryBundleBuilder larger(stage, scratch);
    ASSERT_TRUE(larger.build(source, compressed, synonyms, length));
    EXPECT_EQ(stage.bytes, fixture(name));
  }
}
TEST_F(CompanionDictionaryBundleBuilder, EveryInputAndOutputFailureAbortsAndPreservesLength) {
  uint64_t length = 0;
  ASSERT_TRUE(builder.build(source, false, true, length));
  const auto reads = source.reads, writes = stage.writes;
  for (unsigned failure = 1; failure <= reads; ++failure) {
    source.reads = 0;
    source.failRead = failure;
    length = 42;
    EXPECT_FALSE(builder.build(source, false, true, length));
    EXPECT_EQ(length, 42u);
    EXPECT_FALSE(stage.sealed);
    EXPECT_TRUE(stage.bytes.empty());
  }
  source.failRead = 0;
  for (unsigned failure = 1; failure <= writes; ++failure) {
    stage.failWrite = failure;
    length = 42;
    EXPECT_FALSE(builder.build(source, false, true, length));
    EXPECT_EQ(length, 42u);
    EXPECT_FALSE(stage.sealed);
    EXPECT_TRUE(stage.bytes.empty());
  }
  stage.failWrite = 0;
  EXPECT_TRUE(builder.build(source, false, true, length));
}
TEST_F(CompanionDictionaryBundleBuilder, RejectsPreflightLimitsCloseFailureAndChangingLength) {
  uint64_t length = 42;
  source.oversized = true;
  EXPECT_FALSE(builder.build(source, false, true, length));
  EXPECT_EQ(stage.begins, 0u);
  source.oversized = false;
  source.failSize = true;
  EXPECT_FALSE(builder.build(source, false, true, length));
  EXPECT_EQ(stage.begins, 0u);
  source.failSize = false;
  source.failClose = true;
  EXPECT_FALSE(builder.build(source, false, true, length));
  EXPECT_FALSE(stage.sealed);
  EXPECT_EQ(length, 42u);
  source.failClose = false;
  source.reads = 0;
  source.changed = true;
  EXPECT_FALSE(builder.build(source, false, true, length));
  EXPECT_FALSE(stage.sealed);
  EXPECT_EQ(length, 42u);
  source.changed = false;
  stage.failBegin = true;
  EXPECT_FALSE(builder.build(source, false, true, length));
  EXPECT_FALSE(stage.sealed);
  stage.failBegin = false;
  stage.failSeal = true;
  EXPECT_FALSE(builder.build(source, false, true, length));
  EXPECT_FALSE(stage.sealed);
  stage.failSeal = false;
  DictionaryBundleBuilder shortScratch(stage, std::span(scratch).first(63));
  EXPECT_FALSE(shortScratch.build(source, false, true, length));
  EXPECT_EQ(length, 42u);
}
