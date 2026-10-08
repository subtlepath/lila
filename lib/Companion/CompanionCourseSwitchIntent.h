#pragma once

#include "CompanionCourseSwitchRequest.h"

namespace companion {
inline constexpr char COURSE_SWITCH_INTENT_PATH[] = "/.crosspoint/companion/course-switch";
inline constexpr char COURSE_SWITCH_INTENT_STAGE[] = "/.crosspoint/companion/course-switch-next";
inline constexpr size_t COURSE_SWITCH_INTENT_SIZE = COURSE_SWITCH_REQUEST_SIZE + 4;
enum class CourseSwitchIntentResult { Ok, Missing, Invalid, Conflict, Corrupt, IoError };

// Caller excludes storage writers. Scratch is borrowed, at least 136 bytes,
// and disjoint from request/output objects. No heap allocation is required.
class CourseSwitchIntent {
 public:
  CourseSwitchIntent(TransferStorage& storage, std::span<uint8_t> scratch) : storage(storage), scratch(scratch) {}
  CourseSwitchIntentResult load(CourseSwitchRequest& output) { return read(COURSE_SWITCH_INTENT_PATH, output); }
  CourseSwitchIntentResult persist(const CourseSwitchRequest& request) {
    if (scratch.size() < COURSE_SWITCH_INTENT_SIZE || !validCourseSwitchRequest(request))
      return CourseSwitchIntentResult::Invalid;
    CourseSwitchRequest saved;
    auto result = load(saved);
    if (result == CourseSwitchIntentResult::Ok) {
      if (saved != request) return CourseSwitchIntentResult::Conflict;
      // A second staging record cannot be silently discarded.
      result = read(COURSE_SWITCH_INTENT_STAGE, saved);
      return result == CourseSwitchIntentResult::Missing ? CourseSwitchIntentResult::Ok
             : result == CourseSwitchIntentResult::Ok    ? CourseSwitchIntentResult::Conflict
                                                         : result;
    }
    if (result != CourseSwitchIntentResult::Missing) return result;
    result = read(COURSE_SWITCH_INTENT_STAGE, saved);
    if (result == CourseSwitchIntentResult::Ok) {
      if (saved != request) return CourseSwitchIntentResult::Conflict;
    } else if (result == CourseSwitchIntentResult::Missing) {
      auto bytes = scratch.first(COURSE_SWITCH_INTENT_SIZE);
      if (!encodeCourseSwitchRequest(request, bytes.first(COURSE_SWITCH_REQUEST_SIZE)))
        return CourseSwitchIntentResult::Invalid;
      const auto crc = courseBindingCrc(bytes.first(COURSE_SWITCH_REQUEST_SIZE));
      for (size_t i = 0; i < 4; ++i) bytes[COURSE_SWITCH_REQUEST_SIZE + i] = static_cast<uint8_t>(crc >> (8 * i));
      if (!storage.write(COURSE_SWITCH_INTENT_STAGE, 0, bytes, true)) return CourseSwitchIntentResult::IoError;
      result = read(COURSE_SWITCH_INTENT_STAGE, saved);
      if (result != CourseSwitchIntentResult::Ok) return result;
      if (saved != request) return CourseSwitchIntentResult::Conflict;
    } else {
      return result;
    }
    uint64_t size = 0;
    const auto status = storage.stat(COURSE_SWITCH_INTENT_PATH, size);
    if (status == FileStatus::Error) return CourseSwitchIntentResult::IoError;
    if (status != FileStatus::Missing) return CourseSwitchIntentResult::Conflict;
    if (!storage.rename(COURSE_SWITCH_INTENT_STAGE, COURSE_SWITCH_INTENT_PATH))
      return CourseSwitchIntentResult::IoError;
    result = load(saved);
    return result == CourseSwitchIntentResult::Ok && saved != request ? CourseSwitchIntentResult::Conflict : result;
  }

 private:
  CourseSwitchIntentResult read(const char* path, CourseSwitchRequest& output) {
    if (scratch.size() < COURSE_SWITCH_INTENT_SIZE) return CourseSwitchIntentResult::Invalid;
    uint64_t size = 0;
    const auto status = storage.stat(path, size);
    if (status == FileStatus::Missing) return CourseSwitchIntentResult::Missing;
    if (status == FileStatus::Error) return CourseSwitchIntentResult::IoError;
    if (size != COURSE_SWITCH_INTENT_SIZE) return CourseSwitchIntentResult::Corrupt;
    auto bytes = scratch.first(COURSE_SWITCH_INTENT_SIZE);
    if (!storage.read(path, 0, bytes)) return CourseSwitchIntentResult::IoError;
    uint32_t crc = 0;
    for (size_t i = 0; i < 4; ++i) crc |= uint32_t(bytes[COURSE_SWITCH_REQUEST_SIZE + i]) << (8 * i);
    if (crc != courseBindingCrc(bytes.first(COURSE_SWITCH_REQUEST_SIZE)) ||
        !decodeCourseSwitchRequest(bytes.first(COURSE_SWITCH_REQUEST_SIZE), output))
      return CourseSwitchIntentResult::Corrupt;
    return CourseSwitchIntentResult::Ok;
  }
  TransferStorage& storage;
  std::span<uint8_t> scratch;
};
}  // namespace companion
