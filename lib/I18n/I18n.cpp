#include "I18n.h"

#include <cstddef>
#include <cstring>

#include "I18nStrings.h"

using namespace i18n_strings;

I18n& I18n::getInstance() {
  static I18n instance;
  return instance;
}

const char* I18n::get(StrId id) const {
  const auto index = static_cast<size_t>(id);
  if (index >= static_cast<size_t>(StrId::_COUNT)) {
    return "???";
  }

  // Use generated helper function - no hardcoded switch needed!
  const LangStrings lang = getLanguageStrings(_language);

  // If bit 15 of the offset is set, apply the offset to the English lookup table
  const uint16_t off = lang.offsets[index];
  if (off & 0x8000) return STRINGS_EN_DATA + (off & 0x7FFF);
  return lang.data + off;
}

void I18n::setLanguage(Language lang) {
  if (lang >= Language::_COUNT) {
    return;
  }
  _language = lang;
}

const char* I18n::getLanguageName(Language lang) const {
  const auto index = static_cast<size_t>(lang);
  if (index >= static_cast<size_t>(Language::_COUNT)) {
    return "???";
  }
  return LANGUAGE_NAMES[index];
}

Language I18n::languageFromCode(const char* code) {
  for (uint8_t i = 0; i < getLanguageCount(); i++) {
    if (strcmp(code, LANGUAGE_CODES[i]) == 0) return static_cast<Language>(i);
  }
  return Language::EN;
}

bool I18n::languageFromTag(std::string_view tag, Language& output) {
  if (tag.empty() || tag.size() > 63) return false;
  for (const auto index : SORTED_LANGUAGE_INDICES) {
    const std::string_view supported(LANGUAGE_TAGS[index]);
    if (tag.size() != supported.size()) continue;
    bool matches = true;
    for (size_t at = 0; at < tag.size(); ++at) {
      const auto lower = [](char byte) { return byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte; };
      if (lower(tag[at]) != lower(supported[at])) {
        matches = false;
        break;
      }
    }
    if (matches) {
      output = static_cast<Language>(index);
      return true;
    }
  }
  return false;
}

std::string_view I18n::languageTag(Language language) {
  for (const auto index : SORTED_LANGUAGE_INDICES) {
    if (static_cast<Language>(index) == language) return LANGUAGE_TAGS[index];
  }
  return {};
}

// Generate character set for a specific language
const char* I18n::getCharacterSet(Language lang) {
  const auto langIndex = static_cast<size_t>(lang);
  if (langIndex >= static_cast<size_t>(Language::_COUNT)) {
    lang = Language::EN;  // Fallback to first language
  }

  return CHARACTER_SETS[static_cast<size_t>(lang)];
}
