#include "core/library/SleepWord.h"

#include "core/session/Exercise.h"

namespace tinta::core::library {

namespace pk = pack;

SleepWord pickSleepWord(const pk::Pack& pack, ProgressStore& progress, const Fsrs& fsrs, const DayNumber today,
                        const uint32_t seed, const uint16_t currentLesson) {
  // The weakest few so far, weakest first.
  float weakest[kSleepWordPool];
  int32_t items[kSleepWordPool];
  uint8_t found = 0;
  progress.forEachRecord([&](const ItemState& state, int32_t index) {
    if (index < 0 || state.isNew() || state.suspended()) return;
    pk::Item item;
    if (!pack.item(static_cast<uint32_t>(index), item) || item.kind != ItemKind::VocabRecognise) return;
    if (session::vulgarItem(pack, item)) return;
    const uint32_t elapsed = today > state.lastDay ? static_cast<uint32_t>(today - state.lastDay) : 0;
    const float r = fsrs.retrievability(state.stabilityDays(), elapsed);
    uint8_t at = found;
    while (at > 0 && weakest[at - 1] > r) --at;
    if (at >= kSleepWordPool) return;
    const uint8_t last = found < kSleepWordPool ? found : static_cast<uint8_t>(kSleepWordPool - 1);
    for (uint8_t j = last; j > at; --j) {
      weakest[j] = weakest[j - 1];
      items[j] = items[j - 1];
    }
    weakest[at] = r;
    items[at] = index;
    if (found < kSleepWordPool) ++found;
  });
  SleepWord out;
  if (found > 0) {
    out.item = items[seed % found];
    out.learnt = true;
    return out;
  }
  // Nothing learnt: a word the current lesson brings.
  pk::Lesson lesson;
  if (!pack.lesson(currentLesson, lesson)) return out;
  for (uint16_t k = 0; k < lesson.newCount && k < lesson.itemCount; ++k) {
    pk::Item item;
    if (!pack.item(lesson.firstItem + k, item) || item.kind != ItemKind::VocabRecognise) continue;
    if (session::vulgarItem(pack, item)) continue;
    out.item = static_cast<int32_t>(lesson.firstItem + k);
    return out;
  }
  return out;
}

uint32_t dueBy(ProgressStore& progress, const DayNumber today, const DayNumber day) {
  if (day < today) return 0;
  // forecast() counts overdue items on its first day.
  uint16_t counts[8] = {};
  const uint16_t days = static_cast<uint16_t>(day - today + 1 < 8 ? day - today + 1 : 8);
  if (!progress.forecast(today, counts, days)) return 0;
  uint32_t total = 0;
  for (uint16_t d = 0; d < days; ++d) total += counts[d];
  return total;
}

}  // namespace tinta::core::library
