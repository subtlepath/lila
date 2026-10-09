#pragma once

#include "HalCoursePackArchive.h"
#include "HalFilenameCodec.h"

namespace companion {
enum class CourseHistoryResult { Ok, Invalid, MissingBaseline, Busy, Corrupt, Incompatible, IoError };

// Retain off stack. Caller excludes namespace/state writers and lends scratch.
// Read-only visitors borrow each verified version until they return.
// Learner files without a baseline need migration.
class HalCoursePackHistory final {
 public:
  using Permission = CoursePackArchive::Permission;
  using Visitor = bool (*)(void*, const ContentManifest&, const char*);
  HalCoursePackHistory(std::span<uint8_t> scratch, Permission permitted, void* context)
      : archive(scratch, permitted, context), permitted(permitted), context(context) {}
  ~HalCoursePackHistory() { closeReaders(); }
  CourseHistoryResult visit(const Identity& course, Visitor visitor, void* visitorContext) {
    if (visiting) return CourseHistoryResult::Busy;
    if (course == Identity{} || !visitor || !courseStateDirectory(course, scope)) return CourseHistoryResult::Invalid;
    visiting = true;
    selected = course;
    hasState = false;
    references = 0;
    if (!guard()) return finish(CourseHistoryResult::Busy);
    if (!selectScope()) return finish(CourseHistoryResult::IoError);
    auto result = scan(false, visitor, visitorContext);
    if (result != CourseHistoryResult::Ok) return finish(result);
    if (!references && hasState) return finish(CourseHistoryResult::MissingBaseline);
    if (references) result = scan(true, visitor, visitorContext);
    return finish(result);
  }
  bool closeReaders() {
    const bool archiveClosed = archive.closeReaders();
    const bool entryClosed = closeEntry();
    const bool directoryClosed = !directory.isOpen() || directory.close();
    return archiveClosed && entryClosed && directoryClosed;
  }

 private:
  HalCoursePackArchive archive;
  Permission permitted;
  void* context;
  HalFile directory, entry;
  std::array<char, COURSE_STATE_DIRECTORY_SIZE> scope{};
  std::array<char, 256> name{};
  Identity selected{};
  Digest hash{};
  uint32_t references = 0;
  bool hasState = false, visiting = false;
  bool guard() const { return permitted && permitted(context); }
  static uint32_t fold(uint32_t codepoint) { return Storage.foldFilenameCodepoint(codepoint); }
  bool closeEntry() { return !entry.isOpen() || entry.close(); }
  bool openDirectory(const char* path) {
    return guard() && closeEntry() && (!directory.isOpen() || directory.close()) &&
           Storage.openFileForReadReusing("COMPANION", path, directory) && guard() && directory.isDirectory() &&
           entry.prepareDirectoryEntry() && guard();
  }
  bool entryName(std::string_view& output) {
    const auto count = entry.getName(name.data(), name.size());
    if (!guard() || !count || count >= name.size() || name[count] != 0) return false;
    output = std::string_view(name.data(), count);
    return hal_filename::valid(output);
  }
  bool selectScope() {
    if (!openDirectory("/tinta/courses")) return false;
    const std::string_view wanted(scope.data() + COURSE_STATE_ROOT.size(), 32);
    bool found = false;
    unsigned steps = 0;
    for (;;) {
      if (!guard() || !closeEntry()) return false;
      const auto next = directory.nextEntry(entry);
      if (!guard() || next == HalDirectoryResult::Error) return false;
      if (next == HalDirectoryResult::End) return closeEntry() && directory.close() && guard() && found;
      std::string_view filename;
      if (!entryName(filename)) return false;
      const auto match = hal_filename::compare(wanted, filename, fold);
      if (match == hal_filename::Comparison::Invalid) return false;
      if (match == hal_filename::Comparison::Equal) {
        if (found || !entry.isDirectory()) return false;
        found = true;
      }
      char alias[13]{};
      if (!entry.getShortName(alias, sizeof(alias)) || !guard()) return false;
      const auto size = strnlen(alias, sizeof(alias));
      if (size == sizeof(alias)) return false;
      if (size) {
        const auto aliasMatch = hal_filename::compare(wanted, std::string_view(alias, size), fold);
        if (aliasMatch == hal_filename::Comparison::Invalid ||
            (aliasMatch == hal_filename::Comparison::Equal && match != hal_filename::Comparison::Equal))
          return false;
      }
      if (++steps == 32) {
        steps = 0;
        vTaskDelay(1);
      }
    }
  }
  bool classify(std::string_view filename, bool& owned, bool& staged) {
    owned = false;
    staged = false;
    auto remaining = filename;
    static constexpr std::string_view PREFIX = "pack-";
    for (const auto expected : PREFIX) {
      uint32_t codepoint = 0;
      if (remaining.empty()) return true;
      if (!hal_filename::next(remaining, codepoint)) return false;
      if (fold(codepoint) != fold(static_cast<uint8_t>(expected))) return true;
    }
    owned = true;
    for (unsigned at = 0; at < hash.size(); ++at) {
      uint8_t byte = 0;
      for (unsigned digit = 0; digit < 2; ++digit) {
        uint32_t codepoint = 0;
        if (!hal_filename::next(remaining, codepoint)) return false;
        codepoint = fold(codepoint);
        unsigned value = 0;
        if (codepoint >= '0' && codepoint <= '9')
          value = codepoint - '0';
        else if (codepoint >= fold('a') && codepoint <= fold('f'))
          value = codepoint - fold('a') + 10;
        else
          return false;
        byte = static_cast<uint8_t>((byte << 4) | value);
      }
      hash[at] = byte;
    }
    if (hash == Digest{}) return false;
    if (hal_filename::compare(remaining, ".tmp", fold) == hal_filename::Comparison::Equal) {
      staged = true;
      return true;
    }
    return hal_filename::compare(remaining, ".ref", fold) == hal_filename::Comparison::Equal;
  }
  CourseHistoryResult scan(bool validate, Visitor visitor, void* visitorContext) {
    if (!openDirectory(scope.data())) return CourseHistoryResult::IoError;
    unsigned steps = 0;
    uint32_t visited = 0;
    for (;;) {
      if (!guard()) return CourseHistoryResult::Busy;
      if (!closeEntry()) return CourseHistoryResult::IoError;
      const auto next = directory.nextEntry(entry);
      if (!guard()) return CourseHistoryResult::Busy;
      if (next == HalDirectoryResult::Error) return CourseHistoryResult::IoError;
      if (next == HalDirectoryResult::End) {
        if (!closeEntry() || !directory.close() || !guard()) return CourseHistoryResult::IoError;
        return !validate || visited == references ? CourseHistoryResult::Ok : CourseHistoryResult::Corrupt;
      }
      std::string_view filename;
      bool owned = false, staged = false;
      if (!entryName(filename) || !classify(filename, owned, staged)) return CourseHistoryResult::Corrupt;
      if (owned) {
        if (entry.isDirectory()) return CourseHistoryResult::Corrupt;
        if (staged) return CourseHistoryResult::Busy;
        if (!validate) {
          if (references == UINT32_MAX) return CourseHistoryResult::Corrupt;
          ++references;
        } else {
          const auto result = archive.open(selected, hash);
          if (result != CourseArchiveResult::Ok)
            return result == CourseArchiveResult::Busy      ? CourseHistoryResult::Busy
                   : result == CourseArchiveResult::IoError ? CourseHistoryResult::IoError
                                                            : CourseHistoryResult::Corrupt;
          const auto* manifest = archive.manifest();
          const auto* path = archive.path();
          if (!manifest || !path || !guard()) return CourseHistoryResult::Busy;
          if (!visitor(visitorContext, *manifest, path)) return CourseHistoryResult::Incompatible;
          if (!guard() || !archive.path()) return CourseHistoryResult::Busy;
          if (!archive.closeReaders()) return CourseHistoryResult::IoError;
          if (visited == UINT32_MAX) return CourseHistoryResult::Corrupt;
          ++visited;
        }
      } else if (!validate) {
        hasState = true;
      }
      if (++steps == 32) {
        steps = 0;
        vTaskDelay(1);
      }
    }
  }
  CourseHistoryResult finish(CourseHistoryResult result) {
    if (!closeReaders())
      result = CourseHistoryResult::IoError;
    else if (!guard())
      result = CourseHistoryResult::Busy;
    if (result != CourseHistoryResult::Ok)
      LOG_ERR("COMPANION", "Course history refused: %u", static_cast<unsigned>(result));
    visiting = false;
    return result;
  }
};
}  // namespace companion
