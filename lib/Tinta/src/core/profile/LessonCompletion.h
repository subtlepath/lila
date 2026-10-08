#pragma once

#include "Profile.h"

namespace tinta::core {
class LessonCompletion {
 public:
  struct MutationJournal {
    void* context = nullptr;
    // Inclusive zero-based range: advancing the profile completes every preceding lesson.
    bool (*persist)(void*, uint16_t first, uint16_t last) = nullptr;
    bool (*recover)(void*) = nullptr;
  };
  // Owner logs errors and outlives this binding.
  void setMutationJournal(MutationJournal journal) { journal_ = journal; }
  bool recover(bool storageAvailable) {
    failed_ = false;
    if (storageAvailable && journal_.recover && !journal_.recover(journal_.context)) failed_ = true;
    return !failed_;
  }
  // Individual completion badges remain in the authoritative completion set.
  static bool project(Profile& profile, uint16_t lessonCount, void* context,
                      bool (*completed)(void*, uint16_t index, bool& output)) {
    if (!completed) return false;
    uint16_t firstUnfinished = lessonCount;
    uint16_t unlocked =
        lessonCount ? profile.unlockedThrough < lessonCount ? profile.unlockedThrough : lessonCount - 1 : 0;
    for (uint32_t index = 0; index < lessonCount; ++index) {
      bool done = false;
      if (!completed(context, static_cast<uint16_t>(index), done)) return false;
      if (!done && firstUnfinished == lessonCount) firstUnfinished = static_cast<uint16_t>(index);
      if (done) {
        const uint16_t next = index + 1 < lessonCount ? static_cast<uint16_t>(index + 1) : static_cast<uint16_t>(index);
        if (next > unlocked) unlocked = next;
      }
    }
    if (firstUnfinished < lessonCount && firstUnfinished > unlocked) unlocked = firstUnfinished;
    profile.currentLesson = firstUnfinished;
    profile.unlockedThrough = unlocked;
    return true;
  }
  bool apply(Profile& profile, uint16_t lesson, uint16_t lessonCount, bool durable = true) {
    if (failed_ || lesson >= lessonCount) return false;
    if (lesson + 1u > profile.currentLesson) {
      if (durable && journal_.persist && !journal_.persist(journal_.context, profile.currentLesson, lesson)) {
        failed_ = true;
        return false;
      }
      profile.currentLesson = static_cast<uint16_t>(lesson + 1u);
    }
    const uint16_t next = profile.currentLesson < lessonCount ? profile.currentLesson : lessonCount - 1;
    if (next > profile.unlockedThrough) profile.unlockedThrough = next;
    return true;
  }

 private:
  MutationJournal journal_;
  bool failed_ = false;
};
}  // namespace tinta::core
