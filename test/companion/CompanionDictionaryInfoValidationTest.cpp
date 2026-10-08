#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "lib/Companion/CompanionDictionaryInfoValidation.h"
using namespace companion;
namespace {
class Source final : public InventoryIndexStorage {
 public:
  std::vector<uint8_t> bytes;
  unsigned reads = 0, failRead = 0;
  bool size(uint64_t& output) override {
    output = bytes.size();
    return true;
  }
  bool read(uint64_t offset, std::span<uint8_t> output) override {
    if (++reads == failRead || offset > bytes.size() || output.size() > bytes.size() - offset) return false;
    std::copy_n(bytes.begin() + offset, output.size(), output.begin());
    return true;
  }
  void set(const std::string& text) {
    bytes.assign(text.begin(), text.end());
    reads = 0;
  }
};
constexpr char HEADER[] = "StarDict's dict ifo file\n";
const std::string BASE =
    std::string(HEADER) + "version=3.0.0\nbookname=Diccionario 中文\nwordcount=2\nidxfilesize=24\n";
class CompanionDictionaryInfoValidation : public testing::Test {
 protected:
  Source source;
  std::array<uint8_t, 64> scratch;
  DictionaryInfoValidation validator{source, scratch};
  void SetUp() override { source.set(BASE); }
};
}  // namespace
TEST_F(CompanionDictionaryInfoValidation, ValidatesUtf8TitlesFlagsVersionsAndEverySmallCacheWidth) {
  source.set(BASE + "idxoffsetbits=32\nsametypesequence=h\nsynwordcount=1\ndescription=extra=value\n");
  DictionaryInfoDetails details;
  for (size_t size = 1; size <= scratch.size(); ++size) {
    DictionaryInfoValidation small(source, std::span(scratch).first(size));
    ASSERT_TRUE(small.validate(details)) << size;
    EXPECT_EQ(details, (DictionaryInfoDetails{2, 24, 1, 300, true, true}));
  }
  source.set(std::string(HEADER) +
             "version=2.4.2\r\nbookname=Old\r\nwordcount=0\r\nidxfilesize=0\r\nsametypesequence=mh\r\n");
  ASSERT_TRUE(validator.validate(details));
  EXPECT_EQ(details.version, 242u);
  EXPECT_FALSE(details.htmlDefinitions);
}
TEST_F(CompanionDictionaryInfoValidation, RejectsDuplicatesInvalidCountsFlagsAndAmbiguousReaderPrefix) {
  for (const char* extra : {"wordcount=2\n", "description=x\ndescription=y\n", "idxoffsetbits=64\n",
                            "sametypesequence=1\n", "synwordcount=4294967296\n", "author=idxoffsetbits\n",
                            "author=sametypesequence\n", "broken\n", "=empty-key\n"}) {
    source.set(BASE + extra);
    DictionaryInfoDetails details{9, 9, 9, 242, true, true};
    EXPECT_FALSE(validator.validate(details)) << extra;
    EXPECT_EQ(details.words, 9u);
  }
  source.set(BASE + "description=" + std::string(2100, 'x') + "\nidxoffsetbits=32\n");
  DictionaryInfoDetails details;
  EXPECT_FALSE(validator.validate(details));
  for (const char* number : {"", "-1", "1x", "4294967296"}) {
    source.set(std::string(HEADER) + "version=3.0.0\nbookname=Name\nwordcount=" + number + "\nidxfilesize=24\n");
    EXPECT_FALSE(validator.validate(details));
  }
}
TEST_F(CompanionDictionaryInfoValidation, RejectsBadUtf8WhitespaceNamesAndMissingFields) {
  DictionaryInfoDetails details;
  for (const char* name : {"", " \t", "\xc2\xa0", "\xe2\x80\x8b", "\xc0\x80", "\xed\xa0\x80"}) {
    source.set(std::string(HEADER) + "version=3.0.0\nbookname=" + name + "\nwordcount=2\nidxfilesize=24\n");
    EXPECT_FALSE(validator.validate(details));
  }
  source.set(BASE);
  source.bytes[0] = 0;
  EXPECT_FALSE(validator.validate(details));
  source.set(std::string(HEADER) + "version=3.0.0\nbookname=Name\nwordcount=2\n");
  EXPECT_FALSE(validator.validate(details));
  source.set("wrong signature\n" + BASE);
  EXPECT_FALSE(validator.validate(details));
  source.set(BASE + std::string(65536, 'x'));
  EXPECT_FALSE(validator.validate(details));
  DictionaryInfoValidation empty(source, {});
  EXPECT_FALSE(empty.validate(details));
}
TEST_F(CompanionDictionaryInfoValidation, EveryReadFailureAndCancellationPreserveOutputAndAllowRetry) {
  DictionaryInfoDetails details;
  ASSERT_TRUE(validator.validate(details));
  const auto reads = source.reads;
  for (unsigned failure = 1; failure <= reads; ++failure) {
    source.reads = 0;
    source.failRead = failure;
    details = {9, 9, 9, 242, true, true};
    EXPECT_FALSE(validator.validate(details)) << failure;
    EXPECT_EQ(details.words, 9u);
  }
  source.failRead = 0;
  ASSERT_TRUE(validator.validate(details));
  unsigned callbacks = 0;
  auto progress = [](void* context) {
    ++*static_cast<unsigned*>(context);
    return false;
  };
  source.set(BASE + "description=" + std::string(3000, 'x') + "\n");
  DictionaryInfoValidation cancelled(source, scratch, progress, &callbacks);
  details = {9, 9, 9, 242, true, true};
  EXPECT_FALSE(cancelled.validate(details));
  EXPECT_EQ(callbacks, 1u);
  EXPECT_EQ(details.words, 9u);
}

TEST_F(CompanionDictionaryInfoValidation, SharedAppleAndFirmwareMetadataFixture) {
  std::ifstream file(INFO_FIXTURE, std::ios::binary);
  source.bytes = {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  ASSERT_GT(source.bytes.size(), 100u);
  DictionaryInfoDetails details;
  ASSERT_TRUE(validator.validate(details));
  EXPECT_EQ(details, (DictionaryInfoDetails{2, 24, 1, 300, true, true}));
}

TEST_F(CompanionDictionaryInfoValidation, Utf8ExtensionKeysUseExactBytesAcrossCacheBoundaries) {
  DictionaryInfoDetails details;
  source.set(BASE + "é=first\ne\xcc\x81=second\n中文=value\n");
  for (size_t width = 1; width <= scratch.size(); ++width) {
    DictionaryInfoValidation small(source, std::span(scratch).first(width));
    ASSERT_TRUE(small.validate(details)) << width;
  }
  source.set(BASE + "é=first\né=duplicate\n");
  EXPECT_FALSE(validator.validate(details));
  source.set(BASE + "e\xcc\x81=first\ne\xcc\x81=duplicate\n");
  EXPECT_FALSE(validator.validate(details));
  source.set(BASE + "\xff=invalid\n");
  EXPECT_FALSE(validator.validate(details));
}
