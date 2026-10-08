#pragma once

#include "CompanionCourseSwitchIntent.h"

namespace companion {
namespace course_switch_detail {
enum class Binding { Missing, Previous, Next, Conflict, Error };
inline Binding binding(TransferStorage& storage, const char* path, const CourseSwitchRequest& request,
                       const ContentManifest& next, std::span<uint8_t> scratch) {
  ContentManifest found;
  bool present = false;
  const auto result = readCourseBinding(storage, path, scratch, found, present);
  if (result == CourseBindingResult::IoError) return Binding::Error;
  if (result != CourseBindingResult::Ok) return Binding::Conflict;
  if (!present) return Binding::Missing;
  if (found == next) return Binding::Next;
  return found.logicalIdentity == request.previousCourse && found.contentHash == request.previousHash
             ? Binding::Previous
             : Binding::Conflict;
}
}  // namespace course_switch_detail

// Only a durable Installing/Committed transfer may call this with its saved consent.
// Caller excludes writers and verifies transfer generation/transaction first.
inline CourseSwitchIntentResult publishSwitchedCourseBinding(TransferStorage& storage,
                                                             const CourseSwitchRequest& request,
                                                             const ContentManifest& next, std::span<uint8_t> scratch) {
  using course_switch_detail::Binding;
  if (scratch.size() < COURSE_SWITCH_INTENT_SIZE || !validCourseSwitchRequest(request) || !validCourseBinding(next) ||
      request.nextCourse != next.logicalIdentity || request.nextHash != next.contentHash)
    return CourseSwitchIntentResult::Invalid;
  CourseSwitchIntent intent(storage, scratch);
  CourseSwitchRequest saved;
  auto result = intent.load(saved);
  if (result != CourseSwitchIntentResult::Ok) return result;
  if (saved != request) return CourseSwitchIntentResult::Conflict;
  if (!storage.verify(ACTIVE_COURSE_PATH, next.length, next.contentHash, scratch))
    return CourseSwitchIntentResult::Conflict;
  auto active = course_switch_detail::binding(storage, COURSE_BINDING_PATH, request, next, scratch);
  const auto backup = course_switch_detail::binding(storage, COURSE_BINDING_BACKUP, request, next, scratch);
  auto stage = course_switch_detail::binding(storage, COURSE_BINDING_STAGE, request, next, scratch);
  if (active == Binding::Error || backup == Binding::Error || stage == Binding::Error)
    return CourseSwitchIntentResult::IoError;
  if (active == Binding::Conflict || backup == Binding::Conflict || stage == Binding::Conflict)
    return CourseSwitchIntentResult::Conflict;
  if (active == Binding::Next) {
    if (stage != Binding::Missing || (backup != Binding::Previous && backup != Binding::Missing))
      return CourseSwitchIntentResult::Conflict;
    return backup == Binding::Missing || storage.remove(COURSE_BINDING_BACKUP) ? CourseSwitchIntentResult::Ok
                                                                               : CourseSwitchIntentResult::IoError;
  }
  if ((active == Binding::Previous && backup != Binding::Missing) ||
      (active == Binding::Missing && backup != Binding::Previous) ||
      (stage != Binding::Missing && stage != Binding::Next))
    return CourseSwitchIntentResult::Conflict;
  if (stage == Binding::Missing) {
    // A lost stage after moving the old binding is not an initial publication.
    if (active != Binding::Previous) return CourseSwitchIntentResult::Conflict;
    auto bytes = scratch.first(COURSE_BINDING_SIZE);
    if (encodeCourseBinding(next, bytes) != COURSE_BINDING_SIZE || !storage.write(COURSE_BINDING_STAGE, 0, bytes, true))
      return CourseSwitchIntentResult::IoError;
    stage = course_switch_detail::binding(storage, COURSE_BINDING_STAGE, request, next, scratch);
    if (stage == Binding::Error) return CourseSwitchIntentResult::IoError;
    if (stage != Binding::Next) return CourseSwitchIntentResult::Conflict;
  }
  if (active == Binding::Previous && !storage.rename(COURSE_BINDING_PATH, COURSE_BINDING_BACKUP))
    return CourseSwitchIntentResult::IoError;
  if (!storage.rename(COURSE_BINDING_STAGE, COURSE_BINDING_PATH)) return CourseSwitchIntentResult::IoError;
  active = course_switch_detail::binding(storage, COURSE_BINDING_PATH, request, next, scratch);
  if (active == Binding::Error) return CourseSwitchIntentResult::IoError;
  if (active != Binding::Next) return CourseSwitchIntentResult::Conflict;
  return storage.remove(COURSE_BINDING_BACKUP) ? CourseSwitchIntentResult::Ok : CourseSwitchIntentResult::IoError;
}
}  // namespace companion
