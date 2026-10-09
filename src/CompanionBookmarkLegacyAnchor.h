#pragma once

#include <ProgressContentAnchorResolver.h>

#include "BookmarkEntry.h"

namespace companion {
// Checked off-stack batch owner. Caller freezes the verified EPUB/card context.
class NativeBookmarkLegacyAnchor final {
 public:
  bool resolve(Epub& epub, BookmarkEntry& entry) {
    if (entry.hasVisibleTextOffset) return entry.computedSpineIndex < epub.getSpineItemsCount();
    if (!resolver.resolve(epub, entry.xpath, anchor)) return false;
    entry.computedSpineIndex = anchor.spineIndex;
    entry.visibleTextOffset = anchor.visibleTextOffset;
    entry.hasVisibleTextOffset = true;
    return true;
  }

 private:
  ProgressContentAnchorResolver resolver;
  XPathContentAnchor anchor{};
};
}  // namespace companion
