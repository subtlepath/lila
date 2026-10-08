#include <gtest/gtest.h>

#include "CompanionReaderPreferenceDependencies.h"

TEST(NativeReaderPreferenceDependenciesTest, RejectsUnsupportedBuiltInSizesWithoutSnapping) {
  companion::NativeReaderPreferenceDependencies dependencies(nullptr, nullptr, nullptr);
  for (const auto size : BUILTIN_READER_POINT_SIZES) {
    EXPECT_TRUE(dependencies.font(0, size, {}, {}));
    EXPECT_TRUE(dependencies.font(1, size, {}, {}));
  }
  EXPECT_FALSE(dependencies.font(0, 18, {}, {}));
  EXPECT_FALSE(dependencies.font(2, 12, {}, {}));
  const std::array<char, 4> name{'T', 'e', 's', 't'};
  companion::Digest hash;
  hash.fill(1);
  EXPECT_FALSE(dependencies.font(0, 12, {}, hash));
  EXPECT_FALSE(dependencies.font(0, 12, name, hash));
  EXPECT_FALSE(dependencies.dictionary(name, hash));
}
TEST(NativeReaderPreferenceDependenciesTest, UsesBoundedStrictLanguageTagsAndRetainsOutputOnFailure) {
  companion::NativeReaderPreferenceDependencies dependencies(nullptr, nullptr, nullptr);
  const std::array<uint8_t, 5> tag{'p', 'T', '-', 'b', 'R'};
  uint8_t value = static_cast<uint8_t>(Language::ES);
  ASSERT_TRUE(dependencies.language(tag, value));
  EXPECT_EQ(value, static_cast<uint8_t>(Language::PT));
  EXPECT_FALSE(dependencies.language(std::span(tag).first(2), value));
  EXPECT_EQ(value, static_cast<uint8_t>(Language::PT));
}
