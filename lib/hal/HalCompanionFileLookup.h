#pragma once

#include <HalStorage.h>
#include <Logging.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "CompanionInventoryPaths.h"
#include "CompanionTransfer.h"

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
    if (!std::all_of(wanted.begin(), wanted.end(), [](unsigned char byte) { return byte < 128; }))
      return error("lookup target must be ASCII");
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
      if (sameName(wanted, std::string_view(name.data(), length)))
        return close() ? CompanionFilePresence::Present : CompanionFilePresence::Error;
      if (++steps == 32) {
        steps = 0;
        vTaskDelay(1);
      }
    }
  }

 private:
  HalFile directory, entry;
  const char* parentPath;
  std::array<char, 256> name{};
  Progress progress;
  void* context;
  static bool sameName(std::string_view wanted, std::string_view found) {
    if (wanted.size() != found.size()) return false;
    for (size_t at = 0; at < wanted.size(); ++at) {
      const auto fold = [](unsigned char byte) { return byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte; };
      if (fold(wanted[at]) != fold(found[at])) return false;
    }
    return true;
  }
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
  bool failure(const char* operation) {
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
