#pragma once

#include <HalDictionaryRemovalReferences.h>

#include <cstring>

#include "CrossPointSettings.h"

class CompanionDictionaryRemovalSettings final : public companion::DictionaryRemovalSettings {
 public:
  explicit CompanionDictionaryRemovalSettings(CrossPointSettings& settings = SETTINGS) : settings(settings) {}
  bool load() override { return settings.loadFromFile(); }
  bool selectedDirectory(std::string_view& output) const override {
    const size_t length = strnlen(settings.dictionaryName, sizeof(settings.dictionaryName));
    if (length == sizeof(settings.dictionaryName)) return false;
    output = {settings.dictionaryName, length};
    return true;
  }
  bool clearAndSave() override {
    settings.dictionaryName[0] = '\0';
    return settings.saveToFile();
  }
  bool save() override { return settings.saveToFile(); }

 private:
  CrossPointSettings& settings;
};
