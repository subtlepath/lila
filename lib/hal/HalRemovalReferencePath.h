#pragma once

#include "CompanionInventoryPaths.h"
#include "HalFileName.h"

namespace companion {
// Pure callback for canonical long paths stored by native navigation. Short-name
// aliases must be resolved by the inventory owner before preparing references.
inline bool removalReferencePathEqual(void*, std::string_view first, std::string_view second) {
  if (!validInventoryPath(first) || !validInventoryPath(second)) return false;
  size_t a = 1, b = 1;
  while (a < first.size() && b < second.size()) {
    size_t aEnd = first.find('/', a), bEnd = second.find('/', b);
    if (aEnd == std::string_view::npos) aEnd = first.size();
    if (bEnd == std::string_view::npos) bEnd = second.size();
    if (HalFileName::compare(first.substr(a, aEnd - a), second.substr(b, bEnd - b)) != FileNameComparison::Equal)
      return false;
    a = aEnd + 1;
    b = bEnd + 1;
  }
  return a > first.size() && b > second.size();
}
}  // namespace companion
