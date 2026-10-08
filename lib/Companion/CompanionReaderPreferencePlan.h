#pragma once

#include "CompanionPreferenceBody.h"

namespace companion {
struct ReaderPreferenceValues {
  uint8_t fontFamily = 0, fontPointSize = 12, lineSpacing = 2, paragraphAlignment = 4;
  uint8_t extraParagraphSpacing = 0, wordSpacing = 100, characterSpacing = 2, screenMargin = 5;
  uint8_t hyphenationEnabled = 1, language = 0, textAntiAliasing = 1, embeddedStyle = 1, focusReadingEnabled = 0;
  std::array<char, 32> sdFontFamilyName{}, dictionaryName{};
  bool operator==(const ReaderPreferenceValues&) const = default;
};
class ReaderPreferenceDependencies {
 public:
  virtual ~ReaderPreferenceDependencies() = default;
  virtual bool language(std::span<const uint8_t> tag, uint8_t& value) = 0;
  virtual bool font(uint8_t family, uint8_t pointSize, std::span<const char> name, std::span<const uint8_t> hash) = 0;
  virtual bool dictionary(std::span<const char> name, std::span<const uint8_t> hash) = 0;
};
enum class ReaderPreferencePlanResult : uint8_t { Ok, InvalidBody, UnavailableDependency };
// Retained by the serialized caller. No result is exposed until all dependencies pass.
class ReaderPreferencePlan final {
 public:
  ReaderPreferencePlanResult run(std::span<const std::span<const uint8_t>> bodies,
                                 const ReaderPreferenceValues& current, ReaderPreferenceDependencies& dependencies) {
    ready = false;
    planned = current;
    fontHash = {};
    dictionaryHash = {};
    uint16_t seen = 0;
    bool selectedFont = false, selectedDictionary = false;
    for (const auto body : bodies) {
      PreferenceBodyView value;
      if (!decodePreferenceBody(body, value)) return ReaderPreferencePlanResult::InvalidBody;
      if (value.key >= 32) continue;
      const auto bit = static_cast<uint16_t>(1U << (value.key - 1));
      if (seen & bit) return ReaderPreferencePlanResult::InvalidBody;
      seen |= bit;
      switch (value.key) {
        case 1:
          planned.sdFontFamilyName = {};
          if (value.hasContent) {
            copyName(value.text, planned.sdFontFamilyName);
            std::copy(value.contentHash.begin(), value.contentHash.end(), fontHash.begin());
            selectedFont = true;
          } else
            planned.fontFamily = static_cast<uint8_t>(value.integer);
          break;
        case 2:
          planned.fontPointSize = static_cast<uint8_t>(value.integer);
          break;
        case 3:
          planned.lineSpacing = static_cast<uint8_t>(value.integer);
          break;
        case 4:
          planned.paragraphAlignment = static_cast<uint8_t>(value.integer);
          break;
        case 5:
          planned.extraParagraphSpacing = static_cast<uint8_t>(value.integer);
          break;
        case 6:
          planned.wordSpacing = static_cast<uint8_t>(value.integer);
          break;
        case 7:
          planned.characterSpacing = static_cast<uint8_t>(value.integer + 2);
          break;
        case 8:
          planned.screenMargin = static_cast<uint8_t>(value.integer);
          break;
        case 9:
          planned.hyphenationEnabled = static_cast<uint8_t>(value.integer);
          break;
        case 10:
          if (!dependencies.language(value.text, planned.language))
            return ReaderPreferencePlanResult::UnavailableDependency;
          break;
        case 11:
          planned.dictionaryName = {};
          if (value.hasContent) {
            copyName(value.text, planned.dictionaryName);
            std::copy(value.contentHash.begin(), value.contentHash.end(), dictionaryHash.begin());
            selectedDictionary = true;
          }
          break;
        case 12:
          planned.textAntiAliasing = static_cast<uint8_t>(value.integer);
          break;
        case 13:
          planned.embeddedStyle = static_cast<uint8_t>(value.integer);
          break;
        case 14:
          planned.focusReadingEnabled = static_cast<uint8_t>(value.integer);
          break;
        default:
          return ReaderPreferencePlanResult::InvalidBody;
      }
    }
    if (seen & 3) {
      const auto end = std::find(planned.sdFontFamilyName.begin(), planned.sdFontFamilyName.end(), '\0');
      if (end == planned.sdFontFamilyName.end()) return ReaderPreferencePlanResult::InvalidBody;
      if (!dependencies.font(planned.fontFamily, planned.fontPointSize,
                             std::span(planned.sdFontFamilyName).first(end - planned.sdFontFamilyName.begin()),
                             selectedFont ? std::span<const uint8_t>(fontHash) : std::span<const uint8_t>{}))
        return ReaderPreferencePlanResult::UnavailableDependency;
    }
    if (selectedDictionary) {
      const auto end = std::find(planned.dictionaryName.begin(), planned.dictionaryName.end(), '\0');
      if (!dependencies.dictionary(std::span(planned.dictionaryName).first(end - planned.dictionaryName.begin()),
                                   dictionaryHash))
        return ReaderPreferencePlanResult::UnavailableDependency;
    }
    ready = true;
    return ReaderPreferencePlanResult::Ok;
  }
  const ReaderPreferenceValues* values() const { return ready ? &planned : nullptr; }

 private:
  static void copyName(std::span<const uint8_t> name, std::array<char, 32>& output) {
    std::copy(name.begin(), name.end(), output.begin());
  }
  ReaderPreferenceValues planned;
  Digest fontHash{}, dictionaryHash{};
  bool ready = false;
};
}  // namespace companion
