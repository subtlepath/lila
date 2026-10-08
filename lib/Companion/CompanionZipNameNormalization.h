#pragma once

#include "CompanionUnicodeUtf8Nfc.h"
#include "CompanionZipLegacyName.h"
#include "CompanionZipPathValidation.h"

namespace companion {
// Decodes header names, validates decoded path grammar, trims a directory's
// slash and normalizes to NFC. All spans must be disjoint. Decoded/scalar banks
// are disposable scratch; output/count are changed only on success.
class ZipNameNormalization final {
 public:
  static bool normalize(std::span<const uint8_t> raw, bool utf8, bool directory, std::span<uint8_t> decoded,
                        std::span<uint32_t> inputScalars, std::span<uint32_t> normalizedScalars,
                        std::span<uint8_t> output, size_t& outputBytes) {
    if (raw.empty() || raw.size() > ZipPathValidation::MAX_BYTES) return false;
    size_t bytes = 0;
    if (utf8) {
      if (raw.size() > decoded.size()) return false;
      std::copy(raw.begin(), raw.end(), decoded.begin());
      bytes = raw.size();
    } else if (!zipCp437ToUtf8(raw, decoded, bytes))
      return false;
    ZipPathValidation grammar;
    ZipPathDetails details;
    if (!grammar.consume(decoded.first(bytes)) || !grammar.finish(directory, details)) return false;
    return UnicodeUtf8Nfc::normalize(decoded.first(details.trimmedBytes), inputScalars, normalizedScalars, output,
                                     outputBytes);
  }
};
}  // namespace companion
