#include <I18n.h>
#include <gtest/gtest.h>

TEST(LanguageTagReducedTest, OmittedLanguageCannotSilentlyUseEnglishFallback) {
  Language selected = Language::ES;
  for (const auto tag : {"es", "pt-BR", "pt-PT", "unsupported"}) {
    EXPECT_FALSE(I18n::languageFromTag(tag, selected));
    EXPECT_EQ(selected, Language::ES);
  }
  ASSERT_TRUE(I18n::languageFromTag("EN", selected));
  EXPECT_EQ(selected, Language::EN);
  EXPECT_EQ(I18n::languageTag(Language::EN), "en");
  EXPECT_TRUE(I18n::languageTag(Language::ES).empty());
  EXPECT_TRUE(I18n::languageTag(Language::PT).empty());
  EXPECT_TRUE(I18n::languageTag(Language::_COUNT).empty());
}
TEST(LanguageTagReducedTest, RetainsStableTagsAndEnumsForAllKnownLanguages) {
  EXPECT_STREQ(LANGUAGE_TAGS[static_cast<size_t>(Language::ES)], "es");
  EXPECT_STREQ(LANGUAGE_TAGS[static_cast<size_t>(Language::PT)], "pt-BR");
  EXPECT_STREQ(LANGUAGE_TAGS[static_cast<size_t>(Language::P2)], "pt-PT");
  EXPECT_EQ(sizeof(SORTED_LANGUAGE_INDICES) / sizeof(SORTED_LANGUAGE_INDICES[0]), 1U);
}
