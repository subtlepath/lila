#pragma once

#include "CompanionCourseSwitchIntent.h"

namespace companion {
inline constexpr size_t COURSE_SWITCH_REPLY_SIZE = 17;
namespace course_switch_detail {
inline bool authorize(TransferStorage& storage, const CourseSwitchRequest& request, std::span<uint8_t> scratch) {
  ContentManifest current;
  bool present = false;
  uint64_t size = 0;
  return readCourseBinding(storage, COURSE_BINDING_PATH, scratch, current, present) == CourseBindingResult::Ok &&
         present && current.logicalIdentity == request.previousCourse && current.contentHash == request.previousHash &&
         storage.stat(COURSE_BINDING_STAGE, size) == FileStatus::Missing &&
         storage.stat(COURSE_BINDING_BACKUP, size) == FileStatus::Missing &&
         storage.verifyCourseSwitchSource(current, request.generation, scratch);
}
}  // namespace course_switch_detail
// Caller authenticates the connection and excludes all other state writers.
inline size_t handleCourseSwitch(TransferStorage& storage, Transfer& transfer, const Identity& generation,
                                 const Identity& owner, std::span<const uint8_t> body, std::span<uint8_t> reply,
                                 std::span<uint8_t> scratch) {
  CourseSwitchRequest request;
  if (reply.size() < COURSE_SWITCH_REPLY_SIZE || !decodeCourseSwitchRequest(body, request)) return 0;
  TransferResult result = TransferResult::Invalid;
  const auto* state = transfer.current();
  const auto* manifest = transfer.contentManifest();
  if (request.generation != generation)
    result = TransferResult::WrongStorage;
  else if (!state || !manifest || transfer.destination() != ACTIVE_COURSE_PATH)
    result = TransferResult::NoTransaction;
  else if (state->owner != owner)
    result = TransferResult::Unauthorized;
  else if (state->transaction != request.transaction || state->storageGeneration != request.generation ||
           manifest->kind != ContentKind::Course || manifest->logicalIdentity != request.nextCourse ||
           manifest->contentHash != request.nextHash)
    result = TransferResult::Invalid;
  else if (state->phase != TransferPhase::Receiving)
    result = TransferResult::Busy;
  else if (!course_switch_detail::authorize(storage, request, scratch))
    result = TransferResult::Invalid;
  else {
    CourseSwitchIntent intent(storage, scratch);
    const auto saved = intent.persist(request);
    result = saved == CourseSwitchIntentResult::Ok         ? TransferResult::Ok
             : saved == CourseSwitchIntentResult::Conflict ? TransferResult::Busy
             : saved == CourseSwitchIntentResult::Corrupt  ? TransferResult::Corrupt
                                                           : TransferResult::IoError;
  }
  reply[0] = static_cast<uint8_t>(result);
  std::copy(request.transaction.begin(), request.transaction.end(), reply.begin() + 1);
  return COURSE_SWITCH_REPLY_SIZE;
}
}  // namespace companion
