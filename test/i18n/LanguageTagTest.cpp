#include <I18n.h>
#include <gtest/gtest.h>

#include <string_view>

TEST(LanguageTagTest, DistinguishesSupportedRegionalTagsWithoutFallback) {
  Language selected = Language::ES;
  ASSERT_TRUE(I18n::languageFromTag("pT-bR", selected));
  EXPECT_EQ(selected, Language::PT);
  ASSERT_TRUE(I18n::languageFromTag("pt-PT", selected));
  EXPECT_EQ(selected, Language::P2);
  for (const auto tag : {"pt", "unsupported", "", "pt_PT", "pt-PT-extra"}) {
    EXPECT_FALSE(I18n::languageFromTag(tag, selected));
    EXPECT_EQ(selected, Language::P2);
  }
}
TEST(LanguageTagTest, AcceptsBoundedInputWithoutNullTermination) {
  Language selected = Language::EN;
  const char bounded[] = {'e', 's', 'X'};
  ASSERT_TRUE(I18n::languageFromTag(std::string_view(bounded, 2), selected));
  EXPECT_EQ(selected, Language::ES);
  EXPECT_FALSE(I18n::languageFromTag(std::string_view(bounded, 3), selected));
  EXPECT_EQ(selected, Language::ES);
}
TEST(LanguageTagTest, EveryGeneratedTagMapsToItsOwnEnumValue) {
  Language selected = Language::EN;
  for (size_t index = 0; index < static_cast<size_t>(Language::_COUNT); ++index) {
    ASSERT_TRUE(I18n::languageFromTag(LANGUAGE_TAGS[index], selected));
    EXPECT_EQ(selected, static_cast<Language>(index));
    EXPECT_EQ(I18n::languageTag(selected), LANGUAGE_TAGS[index]);
  }
  EXPECT_TRUE(I18n::languageTag(Language::_COUNT).empty());
  EXPECT_TRUE(I18n::languageTag(static_cast<Language>(255)).empty());
}
