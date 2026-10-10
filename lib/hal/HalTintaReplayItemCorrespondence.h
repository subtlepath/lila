#pragma once

#include "HalTintaLegacyItemCatalogValidation.h"
#include "HalTintaReplayStore.h"

namespace companion {
namespace replay_item_correspondence {
inline bool headerMatches(HalTintaReplayStore& store, const tinta::core::ProgressHeader& header) {
  TintaReplayDay totals;
  return store.day(header.statDay, totals) && header.statNew == std::min<uint32_t>(totals.newItems, UINT16_MAX) &&
         header.statReviews == std::min<uint32_t>(totals.reviews, UINT16_MAX);
}
// Separate walks keep their item/iterator locals out of the view-owning frame.
[[gnu::noinline]] inline bool retainedMatches(HalTintaLegacyItemView& view, HalTintaReplayStore& store,
                                              CourseUidLookup& catalog, std::span<uint8_t> seen,
                                              bool (*permitted)(void*), void* context) {
  for (uint32_t slot = 0; slot < view.count(); ++slot) {
    if (slot % 32 == 0) vTaskDelay(1);
    tinta::core::ItemState retained, projected;
    if (!permitted(context) || !view.record(slot, retained)) return false;
    if (retained.uid == UINT32_MAX) continue;
    int32_t index = -1;
    if (!catalog.find(retained.uid, index) || index < 0 || static_cast<uint32_t>(index) >= catalog.count() ||
        !store.item(retained.uid, projected) || retained != projected)
      return false;
    seen[index / 8] |= uint8_t(1u << (index % 8));
  }
  return true;
}
[[gnu::noinline]] inline bool projectedMatches(HalTintaReplayStore& store, CourseUidLookup& catalog,
                                               std::span<const uint8_t> seen, bool (*permitted)(void*), void* context) {
  uint32_t key = 0;
  bool previous = false, found = false;
  for (;;) {
    if (!permitted(context) || !store.nextKey(HalTintaReplayStore::Kind::Item, previous, key, key, found)) return false;
    if (!found) break;
    int32_t index = -1;
    tinta::core::ItemState projected;
    if (!catalog.find(key, index) || index < 0 || static_cast<uint32_t>(index) >= catalog.count() ||
        !store.item(key, projected))
      return false;
    if (!(seen[index / 8] & uint8_t(1u << (index % 8))) && projected != tinta::core::ItemState::fresh(key))
      return false;
    previous = true;
  }
  return true;
}
}  // namespace replay_item_correspondence
// Caller supplies a hash-verified immutable file, validated catalog and frozen
// replay projection. This proves item states and current-day header counters,
// not legacy provenance, undo-header state or saved-session receipt binding.
inline bool compareTintaReplayItems(HalFile& file, HalTintaReplayStore& store, const Identity& course,
                                    CourseUidLookup& catalog, std::span<uint8_t> scratch, bool (*permitted)(void*),
                                    void* context) {
  const auto failure = []([[maybe_unused]] const char* reason) {
    LOG_ERR("COMPANION", "Replay item correspondence refused: %s", reason);
    return false;
  };
  if (!permitted || !permitted(context) || !store.matchesCourse(course)) return failure("binding");
  LegacyItemCatalogReport report;
  if (!inspectTintaLegacyItemCatalog(file, catalog, scratch, report, permitted, context) || report.retired)
    return failure("catalog");
  HalTintaLegacyItemView view(file);
  if (!view.begin(scratch.first(160), permitted, context) || !view.header() ||
      !replay_item_correspondence::headerMatches(store, *view.header()))
    return failure("header");
  const auto length = file.fileSize64();
  const auto seen = scratch.subspan(160, (uint64_t(catalog.count()) + 7) / 8);
  std::fill(seen.begin(), seen.end(), 0);
  if (!replay_item_correspondence::retainedMatches(view, store, catalog, seen, permitted, context) ||
      !replay_item_correspondence::projectedMatches(store, catalog, seen, permitted, context))
    return failure("item states or coverage");
  return (file.fileSize64() == length && catalog.valid() && view.header() && permitted(context)) ||
         failure("extent or cancellation");
}
}  // namespace companion
