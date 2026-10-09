#if LILA_TINTA
#include "HalCourseStateMigration.h"

#include <HalStorage.h>
#include <Logging.h>

#include "CompanionCourseMigrationProof.h"
#include "CompanionLegacyCourseStateMigration.h"
#include "HalTransferStorage.h"

namespace companion {
bool selectActiveCourseState(HalTransferStorage& storage, std::span<uint8_t> scratch, Identity& course, bool& bound) {
  ContentManifest manifest;
  bool present = false;
  if (readCourseBinding(storage, COURSE_BINDING_PATH, scratch, manifest, present) != CourseBindingResult::Ok) {
    LOG_ERR("COMPANION", "Cannot read active course binding");
    return false;
  }
  if (!present) {
    for (const auto* path :
         {COURSE_STATE_MIGRATION, COURSE_STATE_MIGRATION_STAGE, COURSE_STATE_MIGRATION_DONE,
          COURSE_STATE_MIGRATION_DONE_STAGE, COURSE_MARK_MIGRATION_PATHS.intent, COURSE_MARK_MIGRATION_PATHS.stage,
          COURSE_MARK_MIGRATION_PATHS.done, COURSE_MARK_MIGRATION_PATHS.doneStage}) {
      uint64_t size = 0;
      if (storage.stat(path, size) != FileStatus::Missing) {
        LOG_ERR("COMPANION", "Unbound course state migration");
        return false;
      }
    }
    course = {};
    bound = false;
    return true;
  }
  Identity originalCourse{};
  if (completedCourseStateIsolation(storage, scratch, originalCourse) == CourseStateMigrationResult::Ok &&
      originalCourse != manifest.logicalIdentity) {
    char directory[COURSE_STATE_DIRECTORY_SIZE];
    if (!storage.verify(ACTIVE_COURSE_PATH, manifest.length, manifest.contentHash, scratch) ||
        !courseStateDirectory(manifest.logicalIdentity, directory) ||
        !Storage.ensureDirectoryExists("/tinta/courses") || !Storage.ensureDirectoryExists(directory)) {
      LOG_ERR("COMPANION", "Cannot select isolated course state");
      return false;
    }
  } else if (!prepareMigratedCourseState(storage, manifest.logicalIdentity, scratch)) {
    return false;
  }
  course = manifest.logicalIdentity;
  bound = true;
  return true;
}
bool prepareMigratedCourseState(HalTransferStorage& storage, const Identity& course, std::span<uint8_t> scratch) {
  char directory[COURSE_STATE_DIRECTORY_SIZE];
  if (scratch.size() < 512 || !courseStateDirectory(course, directory)) {
    LOG_ERR("COMPANION", "Invalid course state migration arguments");
    return false;
  }
  if (!storage.prepare() || !Storage.ensureDirectoryExists("/tinta") ||
      !Storage.ensureDirectoryExists("/tinta/courses") || !Storage.ensureDirectoryExists(directory)) {
    LOG_ERR("COMPANION", "Cannot prepare course state directory");
    return false;
  }
  auto result = migrateLegacyCourseState(storage, course, scratch);
  if (result == CourseStateMigrationResult::Ok) result = migrateLegacyCourseMarks(storage, course, scratch);
  if (result != CourseStateMigrationResult::Ok) {
    LOG_ERR("COMPANION", "Course state migration failed: %u", static_cast<unsigned>(result));
    return false;
  }
  return true;
}
}  // namespace companion
#endif
