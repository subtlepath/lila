#pragma once
#include <cstring>

#include "lib/hal/HalFontRemovalReferences.h"
struct CrossPointSettings : companion::FontRemovalSettings {
  char sdFontFamilyName[32] = "Family";
  char persisted[32] = "Family";
  bool saveSucceeds = true;
  bool loadSucceeds = true;
  unsigned saves = 0;
  bool saveToFile() {
    ++saves;
    if (!saveSucceeds) return false;
    std::memcpy(persisted, sdFontFamilyName, sizeof(persisted));
    return true;
  }
  bool clearSdFontFamily() {
    sdFontFamilyName[0] = 0;
    return saveToFile();
  }
  bool loadFromFile() {
    if (!loadSucceeds) return false;
    std::memcpy(sdFontFamilyName, persisted, sizeof(persisted));
    return true;
  }
  bool load() override { return loadFromFile(); }
  bool selectedFamily(std::string_view& output) const override {
    const size_t length = strnlen(sdFontFamilyName, sizeof(sdFontFamilyName));
    if (length == sizeof(sdFontFamilyName)) return false;
    output = {sdFontFamilyName, length};
    return true;
  }
  bool clearAndSave() override { return clearSdFontFamily(); }
  bool save() override { return saveToFile(); }
};
inline CrossPointSettings fontRecoverySettings;
#define SETTINGS fontRecoverySettings
