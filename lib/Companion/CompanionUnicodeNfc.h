#pragma once

#include <algorithm>
#include <iterator>
#include <span>

#include "CompanionUnicodeData.inc"

namespace companion {
// Scalar-level NFC, Unicode 15.1. Input/workspace must not overlap. Workspace
// may contain partial results on failure; the returned count is success-only.
class UnicodeNfc final {
 public:
  static bool normalize(std::span<const uint32_t> input, std::span<uint32_t> workspace, size_t& outputCount) {
    size_t count = 0;
    for (const auto scalar : input) {
      if (scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) return false;
      if (scalar >= 0xac00 && scalar < 0xd7a4) {
        const auto index = scalar - 0xac00;
        if (!append(0x1100 + index / 588, workspace, count) || !append(0x1161 + (index % 588) / 28, workspace, count) ||
            (index % 28 && !append(0x11a7 + index % 28, workspace, count)))
          return false;
      } else {
        const auto* mapping = decomposition(scalar);
        if (mapping) {
          for (unsigned i = 0; i < mapping->count(); ++i)
            if (!append(unicode_data::scalarAt(mapping->offset() + i), workspace, count)) return false;
        } else if (!append(scalar, workspace, count))
          return false;
      }
    }
    size_t written = 0, starter = 0;
    bool haveStarter = false;
    uint8_t previousClass = 0;
    for (size_t at = 0; at < count; ++at) {
      const auto scalar = workspace[at];
      const auto currentClass = combining(scalar);
      const auto combined =
          haveStarter && (previousClass == 0 || previousClass < currentClass) ? compose(workspace[starter], scalar) : 0;
      if (combined)
        workspace[starter] = combined;
      else {
        if (!currentClass) {
          starter = written;
          haveStarter = true;
        }
        workspace[written++] = scalar;
        previousClass = currentClass;
      }
    }
    outputCount = written;
    return true;
  }

 private:
  static uint8_t combining(uint32_t scalar) {
    const auto* begin = std::begin(unicode_data::CLASSES);
    const auto* end = std::end(unicode_data::CLASSES);
    const auto* found =
        std::lower_bound(begin, end, scalar, [](const auto& entry, uint32_t key) { return entry.scalar() < key; });
    return found != end && found->scalar() == scalar ? found->value() : 0;
  }
  static const unicode_data::Decomposition* decomposition(uint32_t scalar) {
    const auto* begin = std::begin(unicode_data::DECOMPOSITIONS);
    const auto* end = std::end(unicode_data::DECOMPOSITIONS);
    const auto* found =
        std::lower_bound(begin, end, scalar, [](const auto& entry, uint32_t key) { return entry.scalar() < key; });
    return found != end && found->scalar() == scalar ? found : nullptr;
  }
  static bool append(uint32_t scalar, std::span<uint32_t> workspace, size_t& count) {
    if (count == workspace.size()) return false;
    size_t at = count++;
    const auto rank = combining(scalar);
    while (rank && at && combining(workspace[at - 1]) > rank) {
      workspace[at] = workspace[at - 1];
      --at;
    }
    workspace[at] = scalar;
    return true;
  }
  static uint32_t compose(uint32_t first, uint32_t second) {
    if (first >= 0x1100 && first < 0x1113 && second >= 0x1161 && second < 0x1176)
      return 0xac00 + (first - 0x1100) * 588 + (second - 0x1161) * 28;
    if (first >= 0xac00 && first < 0xd7a4 && (first - 0xac00) % 28 == 0 && second > 0x11a7 && second < 0x11c3)
      return first + second - 0x11a7;
    const uint64_t key = (static_cast<uint64_t>(first) << 21) | second;
    const auto* begin = std::begin(unicode_data::COMPOSITIONS);
    const auto* end = std::end(unicode_data::COMPOSITIONS);
    const auto* found =
        std::lower_bound(begin, end, key, [](const auto& entry, uint64_t value) { return entry.pair() < value; });
    return found != end && found->pair() == key ? found->scalar() : 0;
  }
};
}  // namespace companion
