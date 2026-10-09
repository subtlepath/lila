#pragma once

#include <HalStorage.h>
#include <Logging.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "CompanionInventoryPaths.h"
#include "CompanionTransfer.h"
#include "HalFilenameCodec.h"

namespace companion {
enum class CompanionFilePresence { Present, Missing, Error };
// Session-owned fixed name buffer and two retained handles. The private parent
// directory must already exist. Absence requires checked end and both closes.
class HalCompanionFileLookup final {
 public:
  using Progress = bool (*)(void*);
  // Parent is borrowed and must outlive the lookup.
  explicit HalCompanionFileLookup(Progress progress = nullptr, void* context = nullptr,
                                  const char* parentPath = TRANSFER_DIRECTORY)
      : parentPath(parentPath), progress(progress), context(context) {}
  ~HalCompanionFileLookup() { close(); }
  CompanionFilePresence inspect(const char* path, bool allowCancellation = true) {
    if (!close() || !Storage.ready() || !path) return error("lookup arguments or close");
    const auto size = strnlen(path, 512);
    const std::string_view view(path, size);
    if (!parentPath) return error("lookup parent missing");
    size_t parentLength = 0;
    while (parentLength < 512 && parentPath[parentLength] != 0) ++parentLength;
    const std::string_view parent(parentPath, parentLength);
    const size_t prefixSize = parent.size() + 1;
    if (parentLength == 512 || !validInventoryPath(parent) || parent.back() == '/' || size == 512 ||
        !validInventoryPath(view) || !view.starts_with(parent) || size <= prefixSize || view[prefixSize - 1] != '/' ||
        view.substr(prefixSize).find('/') != std::string_view::npos)
      return error("lookup path is not a companion child");
    const auto wanted = view.substr(prefixSize);
    if (!hal_filename::valid(wanted)) return error("lookup invalid target name");
    if (!Storage.openFileForReadReusing("COMPANION", parentPath, directory) || !directory.isDirectory() ||
        !entry.prepareDirectoryEntry())
      return error("lookup parent open");
    unsigned steps = 0;
    for (;;) {
      if (allowCancellation && progress && !progress(context)) return error("lookup cancelled");
      if (!closeEntry()) return error("lookup entry close");
      const auto result = directory.nextEntry(entry);
      if (result == HalDirectoryResult::Error) return error("lookup enumeration");
      if (result == HalDirectoryResult::End)
        return close() ? CompanionFilePresence::Missing : CompanionFilePresence::Error;
      const auto length = entry.getName(name.data(), name.size());
      if (length == 0 || length >= name.size() || name[length] != 0) return error("lookup entry name");
      const auto comparison = hal_filename::compare(wanted, std::string_view(name.data(), length), &foldName);
      if (comparison == hal_filename::Comparison::Invalid) return error("lookup invalid entry name");
      if (comparison == hal_filename::Comparison::Equal)
        return close() ? CompanionFilePresence::Present : CompanionFilePresence::Error;
      char alias[13]{};
      if (!entry.getShortName(alias, sizeof(alias))) return error("lookup entry alias");
      const auto aliasLength = strnlen(alias, sizeof(alias));
      if (aliasLength == sizeof(alias)) return error("lookup unterminated alias");
      if (aliasLength != 0) {
        const auto aliasComparison = hal_filename::compare(wanted, std::string_view(alias, aliasLength), &foldName);
        if (aliasComparison == hal_filename::Comparison::Invalid) return error("lookup invalid alias");
        if (aliasComparison == hal_filename::Comparison::Equal)
          return close() ? CompanionFilePresence::Present : CompanionFilePresence::Error;
      }
      if (++steps == 32) {
        steps = 0;
        vTaskDelay(1);
      }
    }
  }

 private:
  HalFile directory, entry;
  const char* parentPath;
  std::array<char, INVENTORY_PATH_LIMIT + 1> name{};
  Progress progress;
  void* context;
  static uint32_t foldName(uint32_t codepoint) { return Storage.foldFilenameCodepoint(codepoint); }
  bool closeEntry() {
    if (entry.isOpen() && !entry.close()) return failure("lookup entry close");
    return true;
  }
  bool close() {
    const bool entryClosed = closeEntry();
    const bool directoryClosed = !directory.isOpen() || directory.close();
    if (!directoryClosed) failure("lookup directory close");
    return entryClosed && directoryClosed;
  }
  bool failure([[maybe_unused]] const char* operation) {
    LOG_ERR("COMPANION", "%s failed", operation);
    return false;
  }
  CompanionFilePresence error(const char* operation) {
    failure(operation);
    close();
    return CompanionFilePresence::Error;
  }
};
}  // namespace companion
