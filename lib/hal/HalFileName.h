#pragma once

#include <common/FsUtf.h>
#include <common/upcase.h>

#include <string_view>

namespace companion {
enum class FileNameComparison { Equal, Different, Invalid };
class HalFileName final {
 public:
  // Pure comparison: uses the same UTF-16 uppercase mapping as FAT/exFAT.
  // No filesystem access, allocation, normalization or full Unicode case folding.
  static FileNameComparison compare(std::string_view first, std::string_view second) {
    if (first.empty() || second.empty()) return FileNameComparison::Invalid;
    const char* a = first.data();
    const char* b = second.data();
    const char* aEnd = a + first.size();
    const char* bEnd = b + second.size();
    bool equal = true;
    while (a != aEnd || b != bEnd) {
      uint32_t left = 0, right = 0;
      if (a != aEnd) {
        a = FsUtf::mbToCp(a, aEnd, &left);
        if (!a || !valid(left)) return FileNameComparison::Invalid;
      } else {
        equal = false;
      }
      if (b != bEnd) {
        b = FsUtf::mbToCp(b, bEnd, &right);
        if (!b || !valid(right)) return FileNameComparison::Invalid;
      } else {
        equal = false;
      }
      // SdFat folds individual UTF-16 units. Surrogate units map to themselves.
      const uint32_t foldedLeft = left <= 0xFFFF ? toUpcase(static_cast<uint16_t>(left)) : left;
      const uint32_t foldedRight = right <= 0xFFFF ? toUpcase(static_cast<uint16_t>(right)) : right;
      equal = equal && foldedLeft == foldedRight;
    }
    return equal ? FileNameComparison::Equal : FileNameComparison::Different;
  }

 private:
  static bool valid(uint32_t scalar) {
    return scalar >= 32 && scalar != 127 && scalar != '/' && scalar != '\\' && scalar != ':';
  }
};
}  // namespace companion
