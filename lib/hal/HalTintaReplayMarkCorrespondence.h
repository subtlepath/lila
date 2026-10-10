#pragma once

#include "HalTintaLegacyCourseReferences.h"
#include "HalTintaReplayStore.h"

namespace companion {
enum class TintaReplayMarkKind { Stars, Readings };
// Caller verifies the immutable mark log and original pack/catalog, excludes
// writers and supplies audited replay. Ambiguous legacy reading keys refuse
// correspondence; no mark log is compacted or replaced.
inline bool compareTintaReplayMarks(HalFile& file, HalTintaReplayStore& store, const Identity& course,
                                    CourseUidLookup& catalog, const tinta::core::pack::Pack& pack,
                                    TintaReplayMarkKind kind, std::span<uint8_t> scratch, bool (*permitted)(void*),
                                    void* context) {
  const auto failure = []([[maybe_unused]] const char* reason) {
    LOG_ERR("COMPANION", "Replay mark correspondence refused: %s", reason);
    return false;
  };
  if (!permitted || !permitted(context) || !store.matchesCourse(course) ||
      (kind != TintaReplayMarkKind::Stars && kind != TintaReplayMarkKind::Readings))
    return failure("binding or kind");
  const bool readings = kind == TintaReplayMarkKind::Readings;
  HalTintaLegacyMarkView marks(file);
  LegacyMarkCatalogReport report;
  if (!marks.begin(scratch, permitted, context) ||
      !inspectTintaLegacyMarkReferences(marks, catalog, pack, readings, report, permitted, context) || report.retired)
    return failure("mark references");
  uint16_t count = 0;
  if (!marks.entryCount(count)) return failure("retained count");
  for (uint16_t at = 0; at < count; ++at) {
    if (at % 32 == 0) vTaskDelay(1);
    uint32_t key = 0;
    if (!permitted(context) || !marks.identityAt(at, key)) return failure("retained identity");
    if (readings) {
      uint32_t uid = 0;
      bool enabled = false;
      if (resolveTintaLegacyStoryKey(pack, key, uid) != LegacyStoryIdentityResult::Matched ||
          !store.completed(EventKind::ReadingComplete, uid, enabled) || !enabled)
        return failure("reading state");
    } else {
      tinta::core::ItemState item;
      if (!store.item(key, item) || !(item.flags & tinta::core::item_flag::kStarred)) return failure("star state");
    }
  }
  uint32_t key = 0, enabledCount = 0;
  bool previous = false, found = false;
  for (;;) {
    if (!permitted(context) ||
        !store.nextKey(readings ? HalTintaReplayStore::Kind::Reading : HalTintaReplayStore::Kind::Item, previous, key,
                       key, found))
      return failure("projection enumeration");
    if (!found) break;
    bool enabled = false;
    if (readings) {
      if (!store.completed(EventKind::ReadingComplete, key, enabled)) return failure("reading lookup");
    } else {
      tinta::core::ItemState item;
      if (!store.item(key, item)) return failure("star lookup");
      enabled = item.flags & tinta::core::item_flag::kStarred;
    }
    if (enabled && ++enabledCount > count) return failure("missing retained mark");
    previous = true;
  }
  uint16_t finalCount = 0;
  return (enabledCount == count && marks.entryCount(finalCount) && finalCount == count && catalog.valid() &&
          permitted(context)) ||
         failure("count, extent or permission");
}
}  // namespace companion
