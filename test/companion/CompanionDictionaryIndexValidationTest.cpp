#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <vector>

#include "lib/Companion/CompanionDictionaryIndexValidation.h"
#include "lib/hal/HalInventoryFileView.h"
using namespace companion;
namespace {
void record(std::vector<uint8_t>& bytes, const char* word, uint32_t offset, uint32_t size, bool synonym = false) {
  while (*word) bytes.push_back(static_cast<uint8_t>(*word++));
  bytes.push_back(0);
  for (unsigned byte = 0; byte < 4; ++byte) bytes.push_back(offset >> (24 - byte * 8));
  if (!synonym)
    for (unsigned byte = 0; byte < 4; ++byte) bytes.push_back(size >> (24 - byte * 8));
}
class CompanionDictionaryIndexValidation : public testing::Test {
 protected:
  DictionaryIndexValidation validator;
  std::vector<uint8_t> bytes;
  void SetUp() override { bytes.reserve(4096); }
  bool stream(size_t chunk) {
    for (size_t offset = 0; offset < bytes.size(); offset += chunk) {
      if (!validator.consume(std::span(bytes).subspan(offset, std::min(chunk, bytes.size() - offset)))) return false;
    }
    return validator.finish();
  }
};
}  // namespace
TEST_F(CompanionDictionaryIndexValidation, PreservesOrderingAcrossAllChunkBoundariesAndUtf8) {
  for (const char* word : {"A", "ab", "ABC", "ABC", "b", "é", "😀"}) record(bytes, word, 0, 3);
  for (size_t chunk = 1; chunk <= bytes.size(); ++chunk) {
    ASSERT_TRUE(validator.begin(7, bytes.size(), DictionaryIndexMode::Definitions, 3));
    ASSERT_TRUE(stream(chunk)) << chunk;
    EXPECT_TRUE(validator.finish());
    EXPECT_TRUE(validator.consume({}));
  }
}
TEST_F(CompanionDictionaryIndexValidation, RejectsDescendingWordsAndShorterEqualPrefixes) {
  for (const auto& pair : {std::pair{"ab", "a"}, std::pair{"b", "a"}, std::pair{"aC", "ab"}}) {
    bytes.clear();
    record(bytes, pair.first, 0, 1);
    record(bytes, pair.second, 0, 1);
    ASSERT_TRUE(validator.begin(2, bytes.size(), DictionaryIndexMode::Definitions, 1));
    EXPECT_FALSE(stream(1));
    EXPECT_FALSE(validator.finish());
    EXPECT_FALSE(validator.consume({}));
  }
}
TEST_F(CompanionDictionaryIndexValidation, ChecksDefinitionAndSynonymOperandsAndCounts) {
  record(bytes, "a", 2, 3);
  ASSERT_TRUE(validator.begin(1, bytes.size(), DictionaryIndexMode::Definitions, 5));
  EXPECT_TRUE(stream(1));
  ASSERT_TRUE(validator.begin(1, bytes.size(), DictionaryIndexMode::Definitions, 4));
  EXPECT_FALSE(stream(1));
  ASSERT_TRUE(validator.begin(0, bytes.size(), DictionaryIndexMode::Definitions, 5));
  EXPECT_FALSE(stream(2));
  ASSERT_TRUE(validator.begin(2, bytes.size(), DictionaryIndexMode::Definitions, 5));
  EXPECT_FALSE(stream(3));
  bytes.clear();
  record(bytes, "alias", 1, 0, true);
  ASSERT_TRUE(validator.begin(1, bytes.size(), DictionaryIndexMode::Synonyms, 2));
  EXPECT_TRUE(stream(1));
  ASSERT_TRUE(validator.begin(1, bytes.size(), DictionaryIndexMode::Synonyms, 1));
  EXPECT_FALSE(stream(1));
  ASSERT_TRUE(validator.begin(1, bytes.size(), DictionaryIndexMode::Synonyms, 0));
  EXPECT_FALSE(stream(1));
  EXPECT_FALSE(validator.begin(1, bytes.size(), DictionaryIndexMode::Synonyms, uint64_t{UINT32_MAX} + 1));
  EXPECT_FALSE(validator.begin(1, bytes.size(), static_cast<DictionaryIndexMode>(255), 5));
}
TEST_F(CompanionDictionaryIndexValidation, RejectsTruncationBadUtf8LongWordsAndPostFinishInput) {
  record(bytes, "a", 0, 1);
  const auto original = bytes;
  for (size_t size = 0; size < original.size(); ++size) {
    bytes.assign(original.begin(), original.begin() + size);
    ASSERT_TRUE(validator.begin(1, size, DictionaryIndexMode::Definitions, 1));
    EXPECT_FALSE(stream(1)) << size;
  }
  for (const char* word : {"", "\xc0\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe2\x82", "\x80"}) {
    bytes.clear();
    record(bytes, word, 0, 1);
    ASSERT_TRUE(validator.begin(1, bytes.size(), DictionaryIndexMode::Definitions, 1));
    EXPECT_FALSE(stream(1));
  }
  std::string word(255, 'a');
  bytes.clear();
  record(bytes, word.c_str(), 0, 1);
  ASSERT_TRUE(validator.begin(1, bytes.size(), DictionaryIndexMode::Definitions, 1));
  EXPECT_TRUE(stream(1));
  const uint8_t extra = 0;
  EXPECT_FALSE(validator.consume(std::span(&extra, 1)));
  EXPECT_FALSE(validator.finish());
  word.push_back('a');
  bytes.clear();
  record(bytes, word.c_str(), 0, 1);
  ASSERT_TRUE(validator.begin(1, bytes.size(), DictionaryIndexMode::Definitions, 1));
  EXPECT_FALSE(stream(1));
  bytes.clear();
  record(bytes, "a", UINT32_MAX, 1);
  ASSERT_TRUE(validator.begin(1, bytes.size(), DictionaryIndexMode::Definitions, UINT32_MAX));
  EXPECT_FALSE(stream(1));
  bytes.clear();
  record(bytes, "a", 0, 0);
  ASSERT_TRUE(validator.begin(1, bytes.size(), DictionaryIndexMode::Definitions, 1));
  EXPECT_FALSE(stream(1));
}

TEST_F(CompanionDictionaryIndexValidation, ValidatesActualHalReadsAndFailsClosedOnIoOrMetadataMismatch) {
  inventory_hal_test::state = {};
  record(bytes, "one", 0, 3);
  record(bytes, "two", 3, 3);
  inventory_hal_test::state.files["/dictionary.idx"] = bytes;
  HalFile file("/dictionary.idx");
  HalInventoryFileView view;
  ASSERT_TRUE(view.attach(file));
  std::array<uint8_t, 3> scratch;
  ASSERT_TRUE(validator.validate(view, scratch, 2, bytes.size(), DictionaryIndexMode::Definitions, 6));
  ASSERT_TRUE(view.attach(file));
  EXPECT_FALSE(validator.validate(view, scratch, 2, bytes.size() + 1, DictionaryIndexMode::Definitions, 6));
  ASSERT_TRUE(view.attach(file));
  inventory_hal_test::state.failRead = inventory_hal_test::state.reads + 2;
  EXPECT_FALSE(validator.validate(view, scratch, 2, bytes.size(), DictionaryIndexMode::Definitions, 6));
  EXPECT_FALSE(validator.finish());
  inventory_hal_test::state.failRead = 0;
  ASSERT_TRUE(view.attach(file));
  EXPECT_TRUE(validator.validate(view, scratch, 2, bytes.size(), DictionaryIndexMode::Definitions, 6));
  EXPECT_FALSE(validator.validate(view, {}, 2, bytes.size(), DictionaryIndexMode::Definitions, 6));
  EXPECT_TRUE(file.isOpen());
}

TEST_F(CompanionDictionaryIndexValidation, FinalSourceSizeFailureCannotBeFinishedAsSuccessful) {
  record(bytes, "a", 0, 1);
  class Source final : public InventoryIndexStorage {
   public:
    const std::vector<uint8_t>& bytes;
    unsigned sizes = 0;
    bool changed = false;
    explicit Source(const std::vector<uint8_t>& bytes) : bytes(bytes) {}
    bool size(uint64_t& output) override {
      ++sizes;
      output = bytes.size() + (changed && sizes == 2 ? 1 : 0);
      return sizes == 1 || changed;
    }
    bool read(uint64_t offset, std::span<uint8_t> output) override {
      if (offset > bytes.size() || output.size() > bytes.size() - offset) return false;
      std::copy_n(bytes.begin() + offset, output.size(), output.begin());
      return true;
    }
  } source(bytes);
  std::array<uint8_t, 3> scratch;
  EXPECT_FALSE(validator.validate(source, scratch, 1, bytes.size(), DictionaryIndexMode::Definitions, 1));
  EXPECT_FALSE(validator.finish());
  source.sizes = 0;
  source.changed = true;
  EXPECT_FALSE(validator.validate(source, scratch, 1, bytes.size(), DictionaryIndexMode::Definitions, 1));
  EXPECT_FALSE(validator.finish());
}

TEST_F(CompanionDictionaryIndexValidation, SharedFirmwareAndAppleIndexFixtures) {
  for (const bool synonyms : {false, true}) {
    std::ifstream input(std::string(INDEX_FIXTURE_DIR) +
                            (synonyms ? "DictionaryIndex-synonyms.fixture" : "DictionaryIndex-definitions.fixture"),
                        std::ios::binary);
    bytes = {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    ASSERT_EQ(bytes.size(), synonyms ? 10u : 24u);
    for (size_t chunk = 1; chunk <= bytes.size(); ++chunk) {
      ASSERT_TRUE(validator.begin(synonyms ? 1 : 2, bytes.size(),
                                  synonyms ? DictionaryIndexMode::Synonyms : DictionaryIndexMode::Definitions,
                                  synonyms ? 2 : 6));
      ASSERT_TRUE(stream(chunk)) << chunk;
    }
  }
}
