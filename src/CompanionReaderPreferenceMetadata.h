#pragma once

#include <I18n.h>

#include "CompanionReaderPreferenceDependencies.h"
#include "CompanionReaderPreferenceEncoding.h"
#include "CompanionReaderPreferenceFontProof.h"
#include "HalReaderPreferenceDictionaryProof.h"

namespace companion {
// Retain off-stack. The caller freezes registry/content mutations and borrows
// disjoint NFC banks, shared scratch and optional compressed-dictionary buffers.
class NativeReaderPreferenceMetadata final {
 public:
  NativeReaderPreferenceMetadata(const SdCardFontRegistry& registry, HalDictionaryBindings& bindings,
                                 std::span<uint8_t> scratch, std::span<uint32_t> decoded,
                                 std::span<uint32_t> normalized, std::span<uint8_t> wanted, std::span<uint8_t> found,
                                 tinfl_decompressor* decoder = nullptr, std::span<uint8_t> window = {},
                                 HalReaderPreferenceDictionaryProof::DecoderProvider decoderProvider = nullptr,
                                 void* decoderContext = nullptr)
      : font(registry, scratch),
        dictionary(bindings, scratch, decoder, window, nullptr, nullptr, decoderProvider, decoderContext),
        lookup(decoded, normalized, wanted, found),
        dependencyProvider(&verifyFont, &verifyDictionary, this) {}
  ReaderPreferenceDependencies& dependencies() { return dependencyProvider; }
  size_t encodePreference(const ReaderPreferenceValues& values, uint8_t key, std::span<uint8_t> output) {
    ready = false;
    std::string_view language;
    std::span<const uint8_t> fontHash, dictionaryHash;
    if (key == 10) {
      language = I18n::languageTag(static_cast<Language>(values.language));
      if (language.empty()) return 0;
    }
    if (key == 1 || key == 2) {
      const auto end = std::find(values.sdFontFamilyName.begin(), values.sdFontFamilyName.end(), '\0');
      if (end == values.sdFontFamilyName.end()) return 0;
      const auto name = std::span(values.sdFontFamilyName).first(end - values.sdFontFamilyName.begin());
      if (!dependencyProvider.font(values.fontFamily, values.fontPointSize, name, {})) return 0;
      if (!name.empty() && key == 1) {
        if (!font.contentHash()) return 0;
        fontHash = *font.contentHash();
      }
    }
    if (key == 11 && values.dictionaryName.front()) {
      const auto end = std::find(values.dictionaryName.begin(), values.dictionaryName.end(), '\0');
      if (end == values.dictionaryName.end() ||
          !dictionary.inspectSelected(lookup,
                                      std::span(values.dictionaryName).first(end - values.dictionaryName.begin())) ||
          !dictionary.contentHash())
        return 0;
      dictionaryHash = *dictionary.contentHash();
    }
    return encodeReaderPreference(values, key, language, fontHash, dictionaryHash, output);
  }
  bool capture(const ReaderPreferenceValues& values) {
    ready = false;
    tag = I18n::languageTag(static_cast<Language>(values.language));
    if (tag.empty()) return failure("language unavailable");
    const auto fontEnd = std::find(values.sdFontFamilyName.begin(), values.sdFontFamilyName.end(), '\0');
    const auto dictionaryEnd = std::find(values.dictionaryName.begin(), values.dictionaryName.end(), '\0');
    if (fontEnd == values.sdFontFamilyName.end() || dictionaryEnd == values.dictionaryName.end())
      return failure("unterminated selection");
    selectedFont = fontEnd != values.sdFontFamilyName.begin();
    selectedDictionary = dictionaryEnd != values.dictionaryName.begin();
    if (selectedFont) {
      if (!font.verify(std::span(values.sdFontFamilyName).first(fontEnd - values.sdFontFamilyName.begin()),
                       values.fontPointSize, {}) ||
          !font.contentHash())
        return failure("font identity");
      fontIdentity = *font.contentHash();
    } else {
      NativeReaderPreferenceDependencies builtins(nullptr, nullptr, nullptr);
      if (!builtins.font(values.fontFamily, values.fontPointSize, {}, {})) return failure("built-in font");
    }
    if (selectedDictionary) {
      if (!dictionary.inspectSelected(
              lookup, std::span(values.dictionaryName).first(dictionaryEnd - values.dictionaryName.begin())) ||
          !dictionary.contentHash())
        return failure("dictionary identity");
      dictionaryIdentity = *dictionary.contentHash();
    }
    ready = true;
    return true;
  }
  std::string_view languageTag() const { return ready ? tag : std::string_view{}; }
  std::span<const uint8_t> fontHash() const {
    return ready && selectedFont ? std::span<const uint8_t>(fontIdentity) : std::span<const uint8_t>{};
  }
  std::span<const uint8_t> dictionaryHash() const {
    return ready && selectedDictionary ? std::span<const uint8_t>(dictionaryIdentity) : std::span<const uint8_t>{};
  }

 private:
  static bool verifyFont(void* context, std::span<const char> name, uint8_t pointSize, std::span<const uint8_t> hash) {
    auto& owner = *static_cast<NativeReaderPreferenceMetadata*>(context);
    owner.ready = false;
    return owner.font.verify(name, pointSize, hash);
  }
  static bool verifyDictionary(void* context, std::span<const char> name, std::span<const uint8_t> hash) {
    auto& owner = *static_cast<NativeReaderPreferenceMetadata*>(context);
    owner.ready = false;
    return owner.dictionary.verifySelected(owner.lookup, name, hash);
  }
  static bool failure(const char* reason) {
    LOG_ERR("COMPANION", "Reader preference metadata unavailable: %s", reason);
    return false;
  }
  NativeReaderPreferenceFontProof font;
  HalReaderPreferenceDictionaryProof dictionary;
  HalDictionaryDestinationLookup lookup;
  NativeReaderPreferenceDependencies dependencyProvider;
  Digest fontIdentity{}, dictionaryIdentity{};
  std::string_view tag;
  bool ready = false, selectedFont = false, selectedDictionary = false;
};
}  // namespace companion
