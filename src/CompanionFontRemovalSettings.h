#pragma once

#include <HalFontRemovalReferences.h>

#include <cstring>

#include "CrossPointSettings.h"

class CompanionFontRemovalSettings final : public companion::FontRemovalSettings {
 public:
  explicit CompanionFontRemovalSettings(CrossPointSettings& settings = SETTINGS) : settings(settings) {}
  bool load() override { return settings.loadFromFile(); }
  bool selectedFamily(std::string_view& output) const override {
    const size_t length = strnlen(settings.sdFontFamilyName, sizeof(settings.sdFontFamilyName));
    if (length == sizeof(settings.sdFontFamilyName)) return false;
    output = {settings.sdFontFamilyName, length};
    return true;
  }
  bool clearAndSave() override { return settings.clearSdFontFamily(); }
  bool save() override { return settings.saveToFile(); }

 private:
  CrossPointSettings& settings;
};
