#pragma once

#include <I18n.h>
#include <Logging.h>

#include "CompanionReaderPreferencePlan.h"
#include "ReaderFontSizes.h"

namespace companion {
// Content callbacks verify the current installed bytes and selected name while
// the serialized caller excludes inventory/content replacement.
class NativeReaderPreferenceDependencies final : public ReaderPreferenceDependencies {
 public:
  using FontProof = bool (*)(void*, std::span<const char>, uint8_t, std::span<const uint8_t>);
  using DictionaryProof = bool (*)(void*, std::span<const char>, std::span<const uint8_t>);
  NativeReaderPreferenceDependencies(FontProof fontProof, DictionaryProof dictionaryProof, void* context)
      : fontProof(fontProof), dictionaryProof(dictionaryProof), context(context) {}
  bool language(std::span<const uint8_t> tag, uint8_t& value) override {
    Language selected;
    if (!I18n::languageFromTag(std::string_view(reinterpret_cast<const char*>(tag.data()), tag.size()), selected)) {
      LOG_ERR("COMPANION", "Reader preference language is unavailable");
      return false;
    }
    value = static_cast<uint8_t>(selected);
    return true;
  }
  bool font(uint8_t family, uint8_t pointSize, std::span<const char> name, std::span<const uint8_t> hash) override {
    if (name.empty()) {
      if (family < 2 && hash.empty() &&
          std::find(std::begin(BUILTIN_READER_POINT_SIZES), std::end(BUILTIN_READER_POINT_SIZES), pointSize) !=
              std::end(BUILTIN_READER_POINT_SIZES))
        return true;
      LOG_ERR("COMPANION", "Reader preference built-in font or size is unavailable");
      return false;
    }
    if (fontProof && fontProof(context, name, pointSize, hash)) return true;
    LOG_ERR("COMPANION", "Reader preference installed font proof failed");
    return false;
  }
  bool dictionary(std::span<const char> name, std::span<const uint8_t> hash) override {
    if (!name.empty() && hash.size() == 32 && dictionaryProof && dictionaryProof(context, name, hash)) return true;
    LOG_ERR("COMPANION", "Reader preference installed dictionary proof failed");
    return false;
  }

 private:
  FontProof fontProof;
  DictionaryProof dictionaryProof;
  void* context;
};
}  // namespace companion
