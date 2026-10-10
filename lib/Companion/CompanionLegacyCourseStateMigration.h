#pragma once

#include <cstdio>
#include <cstring>

#include "CompanionCourseBinding.h"
#include "CompanionCourseStatePaths.h"

namespace companion {
inline constexpr char COURSE_STATE_MIGRATION[] = "/.crosspoint/companion/course-state-migration";
inline constexpr char COURSE_STATE_MIGRATION_STAGE[] = "/.crosspoint/companion/course-state-migration.tmp";
inline constexpr char COURSE_STATE_MIGRATION_DONE[] = "/.crosspoint/companion/course-state-migration.done";
inline constexpr char COURSE_STATE_MIGRATION_DONE_STAGE[] = "/.crosspoint/companion/course-state-migration.done.tmp";
enum class CourseStateMigrationResult { Ok, InvalidBinding, IoError, Corrupt, Conflict };
struct CourseMigrationPaths {
  const char* intent;
  const char* stage;
  const char* done;
  const char* doneStage;
};
inline constexpr CourseMigrationPaths COURSE_MARK_MIGRATION_PATHS{
    "/.crosspoint/companion/course-mark-migration", "/.crosspoint/companion/course-mark-migration.tmp",
    "/.crosspoint/companion/course-mark-migration.done", "/.crosspoint/companion/course-mark-migration.done.tmp"};
inline constexpr CourseMigrationPaths COURSE_STATE_MIGRATION_PATHS{COURSE_STATE_MIGRATION, COURSE_STATE_MIGRATION_STAGE,
                                                                   COURSE_STATE_MIGRATION_DONE,
                                                                   COURSE_STATE_MIGRATION_DONE_STAGE};
inline constexpr const char* COURSE_STATE_MIGRATION_FILES[] = {
    "items.bin",       "items.bin.tmp", "reviews.log",  "reviews.log.tmp", "profile.bin",
    "profile.bin.tmp", "days.bin",      "days.bin.tmp", "session.bin",     "session.bin.tmp"};
inline constexpr const char* COURSE_MARK_MIGRATION_FILES[] = {"starred.bin", "starred.bin.tmp", "read.bin",
                                                              "read.bin.tmp"};
[[gnu::noinline]] inline bool verifyLegacyCourseMigrationBinding(TransferStorage& storage, const Identity& course,
                                                                 std::span<uint8_t> scratch) {
  ContentManifest binding;
  bool bound = false;
  return readCourseBinding(storage, COURSE_BINDING_PATH, scratch, binding, bound) == CourseBindingResult::Ok && bound &&
         binding.logicalIdentity == course &&
         storage.verify(ACTIVE_COURSE_PATH, binding.length, binding.contentHash, scratch);
}
// Caller creates the course directory and excludes all state writers throughout this operation.
inline CourseStateMigrationResult migrateLegacyCourseFiles(TransferStorage& storage, const Identity& course,
                                                           std::span<uint8_t> scratch,
                                                           std::span<const char* const> files,
                                                           const CourseMigrationPaths& paths) {
  if (scratch.size() < 512) return CourseStateMigrationResult::IoError;
  if (files.empty() || files.size() > 15) return CourseStateMigrationResult::InvalidBinding;
  if (!verifyLegacyCourseMigrationBinding(storage, course, scratch)) return CourseStateMigrationResult::InvalidBinding;
  uint64_t size = 0;
  uint16_t expectedFiles = 0;
  const auto intent = storage.stat(paths.intent, size);
  if (intent == FileStatus::Error) return CourseStateMigrationResult::IoError;
  if (intent == FileStatus::Present) {
    if (size != 28 || !storage.read(paths.intent, 0, scratch.first(28))) return CourseStateMigrationResult::Corrupt;
    const uint32_t crc = uint32_t(scratch[24]) | (uint32_t(scratch[25]) << 8) | (uint32_t(scratch[26]) << 16) |
                         (uint32_t(scratch[27]) << 24);
    expectedFiles = uint16_t(scratch[20]) | (uint16_t(scratch[21]) << 8);
    if (std::memcmp(scratch.data(), "CLSM", 4) != 0 || crc != courseBindingCrc(scratch.first(24)) || scratch[22] != 0 ||
        scratch[23] != 0 || expectedFiles >= (1u << files.size()))
      return CourseStateMigrationResult::Corrupt;
    if (!std::equal(course.begin(), course.end(), scratch.begin() + 4)) return CourseStateMigrationResult::Conflict;
  } else {
    for (size_t index = 0; index < files.size(); ++index) {
      const auto* name = files[index];
      char destination[COURSE_STATE_PATH_SIZE];
      if (!courseStatePath(course, name, destination)) return CourseStateMigrationResult::InvalidBinding;
      const auto status = storage.stat(destination, size);
      if (status == FileStatus::Error) return CourseStateMigrationResult::IoError;
      if (status == FileStatus::Present) return CourseStateMigrationResult::Conflict;
      char source[40];
      std::snprintf(source, sizeof(source), "/tinta/%s", name);
      const auto before = storage.stat(source, size);
      if (before == FileStatus::Error) return CourseStateMigrationResult::IoError;
      if (before == FileStatus::Present) expectedFiles |= uint16_t(1u << index);
    }
    std::memcpy(scratch.data(), "CLSM", 4);
    std::copy(course.begin(), course.end(), scratch.begin() + 4);
    scratch[20] = static_cast<uint8_t>(expectedFiles);
    scratch[21] = static_cast<uint8_t>(expectedFiles >> 8);
    scratch[22] = scratch[23] = 0;
    const auto crc = courseBindingCrc(scratch.first(24));
    for (unsigned at = 0; at < 4; ++at) scratch[24 + at] = static_cast<uint8_t>(crc >> (8 * at));
    if (!storage.write(paths.stage, 0, scratch.first(28), true) || !storage.rename(paths.stage, paths.intent))
      return CourseStateMigrationResult::IoError;
  }
  const auto completed = storage.stat(paths.done, size);
  if (completed == FileStatus::Error) return CourseStateMigrationResult::IoError;
  if (completed == FileStatus::Present) {
    if (size != 24 || !storage.read(paths.done, 0, scratch.first(24))) return CourseStateMigrationResult::Corrupt;
    const uint32_t crc = uint32_t(scratch[20]) | (uint32_t(scratch[21]) << 8) | (uint32_t(scratch[22]) << 16) |
                         (uint32_t(scratch[23]) << 24);
    if (std::memcmp(scratch.data(), "CLSD", 4) != 0 || crc != courseBindingCrc(scratch.first(20)))
      return CourseStateMigrationResult::Corrupt;
    return std::equal(course.begin(), course.end(), scratch.begin() + 4) ? CourseStateMigrationResult::Ok
                                                                         : CourseStateMigrationResult::Conflict;
  }
  for (size_t index = 0; index < files.size(); ++index) {
    const auto* name = files[index];
    char source[40], destination[COURSE_STATE_PATH_SIZE];
    std::snprintf(source, sizeof(source), "/tinta/%s", name);
    if (!courseStatePath(course, name, destination)) return CourseStateMigrationResult::InvalidBinding;
    const auto before = storage.stat(source, size), after = storage.stat(destination, size);
    if (before == FileStatus::Error || after == FileStatus::Error) return CourseStateMigrationResult::IoError;
    if (before == FileStatus::Present && after == FileStatus::Present) return CourseStateMigrationResult::Conflict;
    const bool expected = (expectedFiles & (1u << index)) != 0;
    if (expected && before == FileStatus::Missing && after == FileStatus::Missing)
      return CourseStateMigrationResult::Corrupt;
    if (!expected && (before == FileStatus::Present || after == FileStatus::Present))
      return CourseStateMigrationResult::Conflict;
    if (before == FileStatus::Present && !storage.rename(source, destination))
      return CourseStateMigrationResult::IoError;
  }
  std::memcpy(scratch.data(), "CLSD", 4);
  std::copy(course.begin(), course.end(), scratch.begin() + 4);
  const auto crc = courseBindingCrc(scratch.first(20));
  for (unsigned at = 0; at < 4; ++at) scratch[20 + at] = static_cast<uint8_t>(crc >> (8 * at));
  return storage.write(paths.doneStage, 0, scratch.first(24), true) && storage.rename(paths.doneStage, paths.done)
             ? CourseStateMigrationResult::Ok
             : CourseStateMigrationResult::IoError;
}
inline CourseStateMigrationResult migrateLegacyCourseState(TransferStorage& storage, const Identity& course,
                                                           std::span<uint8_t> scratch) {
  return migrateLegacyCourseFiles(storage, course, scratch, COURSE_STATE_MIGRATION_FILES, COURSE_STATE_MIGRATION_PATHS);
}
inline CourseStateMigrationResult migrateLegacyCourseMarks(TransferStorage& storage, const Identity& course,
                                                           std::span<uint8_t> scratch) {
  return migrateLegacyCourseFiles(storage, course, scratch, COURSE_MARK_MIGRATION_FILES, COURSE_MARK_MIGRATION_PATHS);
}
}  // namespace companion
