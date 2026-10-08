#include <gtest/gtest.h>

#include "CompanionReaderPreferenceStore.h"

TEST(NativeReaderPreferenceStoreTest, DurableProofIsRequiredBeforeChangingSettingsOrLanguage) {
  CrossPointSettings settings;
  companion::ReaderPreferenceValues original, replacement;
  settings.readPortablePreferences(original);
  replacement = original;
  replacement.language = static_cast<uint8_t>(Language::PT);
  replacement.wordSpacing = 150;
  I18N.setLanguage(Language::ES);
  bool allowed = false;
  companion::NativeReaderPreferenceStore store(
      settings, [](void* context, const companion::ReaderPreferenceValues&) { return *static_cast<bool*>(context); },
      &allowed);
  EXPECT_EQ(store.replace(original, replacement), companion::ReaderPreferenceStoreResult::IoError);
  EXPECT_EQ(settings.values, original);
  EXPECT_EQ(I18N.getLanguage(), Language::ES);
  allowed = true;
  ASSERT_EQ(store.replace(original, replacement), companion::ReaderPreferenceStoreResult::Ok);
  EXPECT_EQ(settings.values, replacement);
  EXPECT_EQ(settings.deviceOnly, 42);
  EXPECT_EQ(I18N.getLanguage(), Language::PT);
}
TEST(NativeReaderPreferenceStoreTest, RefusesStaleSnapshotAfterProofWithoutOverwritingNewerSettings) {
  CrossPointSettings settings;
  const auto original = settings.values;
  auto replacement = original;
  replacement.screenMargin = 40;
  I18N.setLanguage(Language::ES);
  companion::NativeReaderPreferenceStore store(
      settings,
      [](void* context, const companion::ReaderPreferenceValues&) {
        static_cast<CrossPointSettings*>(context)->values.screenMargin = 30;
        return true;
      },
      &settings);
  EXPECT_EQ(store.replace(original, replacement), companion::ReaderPreferenceStoreResult::Conflict);
  EXPECT_EQ(settings.values.screenMargin, 30);
  EXPECT_EQ(I18N.getLanguage(), Language::ES);
}
