#pragma once
#include "CompanionReaderPreferenceApplication.h"
class CrossPointSettings {
 public:
  companion::ReaderPreferenceValues values;
  uint8_t deviceOnly = 42;
  void readPortablePreferences(companion::ReaderPreferenceValues& output) const { output = values; }
  companion::ReaderPreferenceStoreResult applyPortablePreferencesIfUnchanged(
      const companion::ReaderPreferenceValues& expected, const companion::ReaderPreferenceValues& replacement) {
    if (values != expected) return companion::ReaderPreferenceStoreResult::Conflict;
    values = replacement;
    return companion::ReaderPreferenceStoreResult::Ok;
  }
};
