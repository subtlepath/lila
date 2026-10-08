#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "lib/Companion/CompanionZipPathValidation.h"
using namespace companion;
namespace {
std::span<const uint8_t> bytes(const std::string& text) {
  return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}
bool validate(const std::string& text, bool directory, size_t width, ZipPathDetails& output) {
  ZipPathValidation validation;
  auto input = bytes(text);
  while (!input.empty()) {
    const auto count = std::min(width, input.size());
    if (!validation.consume(input.first(count))) return false;
    input = input.subspan(count);
  }
  return validation.finish(directory, output);
}
}  // namespace
TEST(ZipPathValidationTest, ValidPathsAndUtf8ScalarsWorkAtEveryChunkBoundary) {
  for (const std::string text :
       {"dictionary.ifo", "folder/dictionary.idx", ".hidden/..name", ".../file", "中文/é😀.ifo",
        "e\xcc\x81/dictionary.dict", "literal%2f/name", "name with spaces/file"}) {
    for (size_t width = 1; width <= text.size(); ++width) {
      ZipPathDetails output;
      ASSERT_TRUE(validate(text, false, width, output)) << text << ":" << width;
      EXPECT_EQ(output.rawBytes, text.size());
      EXPECT_EQ(output.trimmedBytes, text.size());
      EXPECT_FALSE(output.trailingSlash);
    }
  }
}
TEST(ZipPathValidationTest, OnlyDirectoriesCanTrimExactlyOneTrailingSlash) {
  for (const std::string text : {"folder/", "中文/é/", "folder/nested/"}) {
    for (size_t width = 1; width <= text.size(); ++width) {
      ZipPathDetails output;
      ASSERT_TRUE(validate(text, true, width, output));
      EXPECT_EQ(output.trimmedBytes, text.size() - 1);
      EXPECT_TRUE(output.trailingSlash);
      EXPECT_FALSE(validate(text, false, width, output));
    }
  }
  ZipPathDetails output;
  EXPECT_TRUE(validate("folder", true, 1, output));
  EXPECT_FALSE(output.trailingSlash);
  EXPECT_FALSE(validate("folder//", true, 1, output));
}
TEST(ZipPathValidationTest, TraversalAbsoluteControlAndPlatformSeparatorsFailInBothModes) {
  std::vector<std::string> invalid = {"",     "/",      "/file",   "//file", "a//b",   ".",
                                      "..",   "./file", "../file", "a/./b",  "a/../b", "a/.",
                                      "a/..", "a\\b",   "C:/file", "a:b",    "a\x7f"};
  invalid.reserve(20);
  invalid.emplace_back("a\0b", 3);
  invalid.emplace_back(
      "a\x1f"
      "b");
  for (const auto& text : invalid) {
    for (bool directory : {false, true}) {
      ZipPathDetails output{99, 98, true};
      EXPECT_FALSE(validate(text, directory, 1, output)) << text;
      EXPECT_EQ(output, (ZipPathDetails{99, 98, true}));
    }
  }
}
TEST(ZipPathValidationTest, InvalidUtf8OverlongSurrogateAndTruncatedScalarsFail) {
  for (const std::string text :
       {"\x80", "\xc0\xaf", "\xc1\x81", "\xe0\x80\xaf", "\xed\xa0\x80", "\xf0\x80\x80\xaf", "\xf4\x90\x80\x80",
        "\xf5\x80\x80\x80", "\xc2", "\xe2\x82", "\xf0\x9f\x98", "\xc2/", "\xe2\x82/file"}) {
    for (size_t width = 1; width <= text.size(); ++width) {
      ZipPathDetails output{99, 98, true};
      EXPECT_FALSE(validate(text, false, width, output));
      EXPECT_EQ(output.rawBytes, 99u);
    }
  }
}
TEST(ZipPathValidationTest, ByteLimitStickyFailureResetAndFinishedStateAreExplicit) {
  ZipPathDetails output;
  EXPECT_TRUE(validate(std::string(1024, 'a'), false, 17, output));
  EXPECT_EQ(output.rawBytes, 1024u);
  EXPECT_FALSE(validate(std::string(1025, 'a'), false, 17, output));
  EXPECT_TRUE(validate(std::string(1023, 'a') + "/", true, 19, output));
  ZipPathValidation validation;
  ASSERT_FALSE(validation.consume(bytes(std::string("../bad"))));
  EXPECT_FALSE(validation.consume(bytes(std::string("good"))));
  EXPECT_FALSE(validation.finish(false, output));
  validation.reset();
  ASSERT_TRUE(validation.consume(bytes(std::string("good"))));
  ASSERT_TRUE(validation.finish(false, output));
  EXPECT_FALSE(validation.consume({}));
  EXPECT_FALSE(validation.finish(false, output));
  validation.reset();
  ASSERT_TRUE(validation.consume(bytes(std::string("中文"))));
  ASSERT_TRUE(validation.finish(false, output));
}

TEST(ZipPathValidationTest, ValidUtf8ScalarBoundariesRemainAccepted) {
  for (const std::string text : {"\xc2\x80", "\xdf\xbf", "\xe0\xa0\x80", "\xed\x9f\xbf", "\xee\x80\x80", "\xef\xbf\xbf",
                                 "\xf0\x90\x80\x80", "\xf4\x8f\xbf\xbf"}) {
    ZipPathDetails output;
    ASSERT_TRUE(validate(text, false, 1, output));
    EXPECT_EQ(output.rawBytes, text.size());
  }
}
