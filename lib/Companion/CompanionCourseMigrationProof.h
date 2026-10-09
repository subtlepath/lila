#pragma once

#include "CompanionLegacyCourseStateMigration.h"

namespace companion {
// Read-only proof. Completion refers to the original course, not the active course.
// Scratch needs 28 bytes and must be disjoint from output. Caller excludes writers.
inline CourseStateMigrationResult completedCourseMigration(TransferStorage& storage, const CourseMigrationPaths& paths,
                                                           std::span<const char* const> files,
                                                           std::span<uint8_t> scratch, Identity& output) {
  if (scratch.size() < 28 || files.empty() || files.size() > 15) return CourseStateMigrationResult::InvalidBinding;
  uint64_t size = 0;
  for (const char* path : {paths.stage, paths.doneStage}) {
    const auto status = storage.stat(path, size);
    if (status == FileStatus::Error) return CourseStateMigrationResult::IoError;
    if (status != FileStatus::Missing) return CourseStateMigrationResult::Conflict;
  }
  const auto intentStatus = storage.stat(paths.intent, size);
  if (intentStatus == FileStatus::Error) return CourseStateMigrationResult::IoError;
  if (intentStatus != FileStatus::Present || size != 28 || !storage.read(paths.intent, 0, scratch.first(28)))
    return CourseStateMigrationResult::Corrupt;
  uint32_t crc = 0;
  for (size_t i = 0; i < 4; ++i) crc |= uint32_t(scratch[24 + i]) << (8 * i);
  const uint16_t mask = uint16_t(scratch[20]) | (uint16_t(scratch[21]) << 8);
  if (std::memcmp(scratch.data(), "CLSM", 4) != 0 || crc != courseBindingCrc(scratch.first(24)) || scratch[22] ||
      scratch[23] || mask >= (1u << files.size()))
    return CourseStateMigrationResult::Corrupt;
  Identity origin;
  std::copy_n(scratch.begin() + 4, origin.size(), origin.begin());
  if (std::none_of(origin.begin(), origin.end(), [](uint8_t byte) { return byte != 0; }))
    return CourseStateMigrationResult::Corrupt;
  const auto doneStatus = storage.stat(paths.done, size);
  if (doneStatus == FileStatus::Error) return CourseStateMigrationResult::IoError;
  if (doneStatus != FileStatus::Present || size != 24 || !storage.read(paths.done, 0, scratch.first(24)))
    return CourseStateMigrationResult::Corrupt;
  crc = 0;
  for (size_t i = 0; i < 4; ++i) crc |= uint32_t(scratch[20 + i]) << (8 * i);
  if (std::memcmp(scratch.data(), "CLSD", 4) != 0 || crc != courseBindingCrc(scratch.first(20)))
    return CourseStateMigrationResult::Corrupt;
  if (!std::equal(origin.begin(), origin.end(), scratch.begin() + 4)) return CourseStateMigrationResult::Conflict;
  for (const auto* name : files) {
    char path[40];
    const int count = std::snprintf(path, sizeof(path), "/tinta/%s", name);
    if (count < 0 || static_cast<size_t>(count) >= sizeof(path)) return CourseStateMigrationResult::InvalidBinding;
    const auto status = storage.stat(path, size);
    if (status == FileStatus::Error) return CourseStateMigrationResult::IoError;
    if (status != FileStatus::Missing) return CourseStateMigrationResult::Conflict;
  }
  output = origin;
  return CourseStateMigrationResult::Ok;
}
// Both migrations must name the same original course. This does not prove that
// the current course directory exists; the caller checks that separately.
inline CourseStateMigrationResult completedCourseStateIsolation(TransferStorage& storage, std::span<uint8_t> scratch,
                                                                Identity& output) {
  Identity stateOrigin{}, markOrigin{};
  auto result = completedCourseMigration(storage, COURSE_STATE_MIGRATION_PATHS, COURSE_STATE_MIGRATION_FILES, scratch,
                                         stateOrigin);
  if (result != CourseStateMigrationResult::Ok) return result;
  result =
      completedCourseMigration(storage, COURSE_MARK_MIGRATION_PATHS, COURSE_MARK_MIGRATION_FILES, scratch, markOrigin);
  if (result != CourseStateMigrationResult::Ok) return result;
  if (stateOrigin != markOrigin) return CourseStateMigrationResult::Conflict;
  output = stateOrigin;
  return CourseStateMigrationResult::Ok;
}
}  // namespace companion
