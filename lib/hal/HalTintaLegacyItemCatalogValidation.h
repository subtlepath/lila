#pragma once

#include "../Companion/CompanionCourseUidLookup.h"
#include "HalTintaLegacyItemView.h"

namespace companion {
struct LegacyItemCatalogReport {
  uint32_t mapped = 0, retired = 0, tombstones = 0;
};
// Inspect immutable, hash-verified reviewed items against a fully validated pack.
// Retired states remain evidence, as in ProgressStore; absence does not prove UID
// meaning. The caller still needs original-pack consent and course continuity.
inline bool inspectTintaLegacyItemCatalog(HalFile& file, CourseUidLookup& catalog, std::span<uint8_t> scratch,
                                          LegacyItemCatalogReport& output, bool (*permitted)(void*) = nullptr,
                                          void* context = nullptr) {
  const auto failure = []([[maybe_unused]] const char* reason) {
    LOG_ERR("COMPANION", "Legacy item catalog inspection failed: %s", reason);
    return false;
  };
  const auto items = catalog.count();
  const size_t bitmapSize = (uint64_t(items) + 7) / 8;
  if (!catalog.valid() || items > 32767 || scratch.size() < 160 + bitmapSize) return failure("workspace");
  HalTintaLegacyItemView view(file);
  if (!view.begin(scratch.first(160), permitted, context)) return failure("items");
  const auto seen = scratch.subspan(160, bitmapSize);
  std::fill(seen.begin(), seen.end(), 0);
  LegacyItemCatalogReport result;
  const auto records = view.count();
  for (uint32_t slot = 0; slot < records; ++slot) {
    tinta::core::ItemState item;
    if (!view.record(slot, item)) return failure("record");
    if (item.uid == UINT32_MAX) {
      ++result.tombstones;
      continue;
    }
    int32_t index = -1;
    if (!catalog.find(item.uid, index) || (permitted && !permitted(context))) return failure("catalog read");
    if (index < 0) {
      ++result.retired;
      continue;
    }
    if (static_cast<uint32_t>(index) >= items) return failure("catalog index");
    const auto mask = uint8_t(1u << (index % 8));
    if (seen[index / 8] & mask) return failure("duplicate UID");
    seen[index / 8] |= mask;
    ++result.mapped;
  }
  const auto* header = view.header();
  if (!header) return failure("cancelled");
  if (header->undoValid) {
    tinta::core::ItemState item;
    if (!view.record(header->undoSlot, item) || item.uid != header->undoBefore.uid) return failure("undo UID");
  }
  if (!catalog.valid() || (permitted && !permitted(context))) return failure("cancelled");
  output = result;
  return true;
}
}  // namespace companion
