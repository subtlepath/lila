#pragma once

#include <string_view>

#include "CompanionReaderPreferencePlan.h"

namespace companion {
// Content identity and canonical language tag come from checked native metadata.
inline size_t encodeReaderPreference(const ReaderPreferenceValues& values, uint8_t key, std::string_view languageTag,
                                     std::span<const uint8_t> fontHash, std::span<const uint8_t> dictionaryHash,
                                     std::span<uint8_t> output) {
  if (output.size() < 69 || key < 1 || key > 14) return 0;
  output[0] = 1;
  output[1] = static_cast<uint8_t>(EventKind::Preference);
  output[2] = key;
  output[3] = 1;
  size_t size = 8;
  int32_t integer = 0;
  const auto content = [&](const std::array<char, 32>& name, std::span<const uint8_t> hash) {
    const auto end = std::find(name.begin(), name.end(), '\0');
    if (end == name.end() || hash.size() != 32) return size_t{0};
    const auto length = static_cast<size_t>(end - name.begin());
    output[3] = 3;
    output[4] = 1;
    std::copy(hash.begin(), hash.end(), output.begin() + 5);
    output[37] = static_cast<uint8_t>(length);
    std::copy_n(name.begin(), length, output.begin() + 38);
    return 38 + length;
  };
  switch (key) {
    case 1:
      if (values.sdFontFamilyName.front())
        size = content(values.sdFontFamilyName, fontHash);
      else
        integer = values.fontFamily;
      break;
    case 2:
      integer = values.fontPointSize;
      break;
    case 3:
      integer = values.lineSpacing;
      break;
    case 4:
      integer = values.paragraphAlignment;
      break;
    case 5:
      integer = values.extraParagraphSpacing;
      break;
    case 6:
      integer = values.wordSpacing;
      break;
    case 7:
      integer = static_cast<int32_t>(values.characterSpacing) - 2;
      break;
    case 8:
      integer = values.screenMargin;
      break;
    case 9:
      integer = values.hyphenationEnabled;
      break;
    case 10:
      if (languageTag.empty() || languageTag.size() > 63) return 0;
      output[3] = 2;
      output[4] = static_cast<uint8_t>(languageTag.size());
      std::copy(languageTag.begin(), languageTag.end(), output.begin() + 5);
      size = 5 + languageTag.size();
      break;
    case 11:
      if (values.dictionaryName.front())
        size = content(values.dictionaryName, dictionaryHash);
      else {
        output[3] = 3;
        output[4] = 0;
        size = 5;
      }
      break;
    case 12:
      integer = values.textAntiAliasing;
      break;
    case 13:
      integer = values.embeddedStyle;
      break;
    case 14:
      integer = values.focusReadingEnabled;
      break;
  }
  if (!size) return 0;
  if (output[3] == 1) {
    const auto number = std::bit_cast<uint32_t>(integer);
    for (unsigned at = 0; at < 4; ++at) output[4 + at] = static_cast<uint8_t>(number >> (8 * at));
  }
  PreferenceBodyView decoded;
  return decodePreferenceBody(output.first(size), decoded) ? size : 0;
}
}  // namespace companion
