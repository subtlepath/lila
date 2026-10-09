#pragma once

#include <optional>

#include "../BookmarkEntry.h"

struct BookmarkTextPageRange {
  std::optional<uint32_t> start, end;
  bool finalPage = false;
};
inline bool bookmarkMatchesTextPage(const BookmarkEntry& bookmark, int spine, const BookmarkTextPageRange& range) {
  if (!bookmark.hasVisibleTextOffset || spine < 0 || spine > UINT16_MAX || bookmark.computedSpineIndex != spine ||
      !range.start || bookmark.visibleTextOffset < *range.start)
    return false;
  if (range.end) return *range.end > *range.start && bookmark.visibleTextOffset < *range.end;
  // A partial watermark proves its start, but not the extent of unseen text.
  return range.finalPage || bookmark.visibleTextOffset == *range.start;
}
