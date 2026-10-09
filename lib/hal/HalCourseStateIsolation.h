#pragma once

#include <HalStorage.h>
#include <Logging.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "CompanionCourseMigrationProof.h"
#include "HalFilenameCodec.h"

namespace companion {
// Retain off stack. Caller excludes learner/metadata writers and lends scratch
// for the entire verification. This owner never creates or mutates state.
class HalCourseStateIsolation final {
 public:
  using Permission = bool (*)(void*);
  HalCourseStateIsolation(TransferStorage& metadata, std::span<uint8_t> scratch, Permission permitted, void* context)
      : metadata(metadata), scratch(scratch), permitted(permitted), context(context) {}
  ~HalCourseStateIsolation() { closeReaders(); }
  bool verify(const Identity& course) {
    if (!closeReaders() || !guard() || !courseStateDirectory(course, path) ||
        completedCourseStateIsolation(metadata, scratch, origin) != CourseStateMigrationResult::Ok || !guard())
      return fail("migration proof");
    if (!Storage.openFileForReadReusing("COMPANION", "/tinta/courses", directory) || !guard() ||
        !directory.isDirectory() || !entry.prepareDirectoryEntry())
      return fail("parent directory");
    const std::string_view wanted(path.data() + COURSE_STATE_ROOT.size(), 32);
    bool found = false;
    unsigned steps = 0;
    for (;;) {
      if (!guard() || !closeEntry()) return fail("enumeration permission or close");
      const auto result = directory.nextEntry(entry);
      if (!guard() || result == HalDirectoryResult::Error) return fail("enumeration");
      if (result == HalDirectoryResult::End) {
        const bool closed = closeReaders();
        return (closed && guard() && found) || fail("missing state directory or close");
      }
      const auto length = entry.getName(name.data(), name.size());
      if (!guard() || !length || length >= name.size() || name[length] != 0) return fail("entry name");
      const auto comparison = hal_filename::compare(wanted, std::string_view(name.data(), length), &foldName);
      if (comparison == hal_filename::Comparison::Invalid) return fail("invalid entry name");
      if (comparison == hal_filename::Comparison::Equal) {
        if (found || !entry.isDirectory() || !guard()) return fail("ambiguous state directory");
        found = true;
      }
      char alias[13]{};
      if (!entry.getShortName(alias, sizeof(alias)) || !guard()) return fail("entry alias");
      const auto aliasLength = strnlen(alias, sizeof(alias));
      if (aliasLength == sizeof(alias)) return fail("unterminated alias");
      if (aliasLength) {
        const auto aliasComparison = hal_filename::compare(wanted, std::string_view(alias, aliasLength), &foldName);
        if (aliasComparison == hal_filename::Comparison::Invalid ||
            (aliasComparison == hal_filename::Comparison::Equal && comparison != hal_filename::Comparison::Equal))
          return fail("conflicting alias");
      }
      if (++steps == 32) {
        steps = 0;
        vTaskDelay(1);
      }
    }
  }
  bool closeReaders() {
    const bool entryClosed = closeEntry();
    const bool directoryClosed = !directory.isOpen() || directory.close();
    return entryClosed && directoryClosed;
  }

 private:
  TransferStorage& metadata;
  std::span<uint8_t> scratch;
  Permission permitted;
  void* context;
  HalFile directory, entry;
  std::array<char, 256> name{};
  std::array<char, COURSE_STATE_DIRECTORY_SIZE> path{};
  Identity origin{};
  bool guard() const { return permitted && permitted(context); }
  bool closeEntry() { return !entry.isOpen() || entry.close(); }
  static uint32_t foldName(uint32_t codepoint) { return Storage.foldFilenameCodepoint(codepoint); }
  bool fail(const char* operation) {
    LOG_ERR("COMPANION", "Course state isolation %s failed", operation);
    closeReaders();
    return false;
  }
};
}  // namespace companion
