#include "core/session/Lessons.h"

#include "core/session/Exercise.h"

namespace tinta::core::session {

uint16_t lessonPractice(const pack::Pack& pack, const uint16_t lesson, const bool showVulgar, uint32_t* out,
                        const uint16_t cap) {
  pack::Lesson record;
  if (!pack.lesson(lesson, record)) return 0;
  uint16_t count = 0;
  const auto add = [&](uint32_t index, const pack::Item& item) {
    if (count >= cap || (!showVulgar && vulgarItem(pack, item))) return;
    out[count++] = index;
  };
  // Pass 0: the new words, in authored order. Pass 1: everything else with no
  // prerequisite.
  for (int pass = 0; pass < 2; ++pass) {
    for (uint32_t k = 0; k < record.itemCount; ++k) {
      const uint32_t index = record.firstItem + k;
      pack::Item item;
      if (!pack.item(index, item)) break;
      const bool newWord = k < record.newCount && item.kind == ItemKind::VocabRecognise;
      if (pass == 0 && newWord) add(index, item);
      if (pass == 1 && !newWord && item.prereq == pack::kNone16) add(index, item);
    }
  }
  return count;
}

}  // namespace tinta::core::session
