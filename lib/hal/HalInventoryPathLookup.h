#pragma once

#include <HalStorage.h>
#include <Logging.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstring>

#include "CompanionInventoryPaths.h"
#include "CompanionTransfer.h"
#include "CompanionZipPathValidation.h"
#include "HalFileName.h"

namespace companion {
// Session-owned name/parent buffers and retained handles. Input remains immutable
// throughout the call; the serialized caller excludes namespace mutations.
class HalInventoryPathLookup final {
 public:
  using Progress = bool (*)(void*);
  explicit HalInventoryPathLookup(Progress progress = nullptr, void* context = nullptr)
      : progress(progress), context(context) {}
  ~HalInventoryPathLookup() { close(); }
  FileStatus stat(const char* path, uint64_t& output) {
    if (!close() || !Storage.ready() || !path) return error("arguments/close");
    const size_t size = strnlen(path, INVENTORY_PATH_LIMIT + 1);
    const std::string_view view(path, size);
    if (!validInventoryPath(view)) return error("path grammar");
    ZipPathValidation grammar;
    ZipPathDetails details;
    if (!grammar.consume(std::span(reinterpret_cast<const uint8_t*>(path + 1), size - 1)) ||
        !grammar.finish(false, details))
      return error("path UTF-8");
    const size_t separator = view.find_last_of('/');
    const auto wanted = view.substr(separator + 1);
    const size_t parentSize = separator ? separator : 1;
    std::copy_n(path, parentSize, parent.begin());
    parent[parentSize] = 0;
    if (!Storage.openFileForReadReusing("COMPANION", parent.data(), directory) || !directory.isDirectory() ||
        !entry.prepareDirectoryEntry())
      return error("parent open");
    unsigned steps = 0;
    for (;;) {
      if ((progress && !progress(context)) || !closeEntry()) return error("cancel/entry close");
      const auto next = directory.nextEntry(entry);
      if (next == HalDirectoryResult::Error) return error("enumeration");
      if (next == HalDirectoryResult::End) {
        if (!close() || (progress && !progress(context))) return error("absence close/cancel");
        return FileStatus::Missing;
      }
      const size_t length = entry.getName(name.data(), name.size());
      if (!length || length >= name.size() || name[length] != 0) return error("entry name");
      const auto match = HalFileName::compare(wanted, std::string_view(name.data(), length));
      if (match == FileNameComparison::Invalid) return error("invalid entry name");
      if (!entry.getShortName(alias.data(), alias.size())) return error("entry alias");
      const size_t aliasLength = strnlen(alias.data(), alias.size());
      if (aliasLength == alias.size()) return error("unterminated alias");
      auto aliasMatch = FileNameComparison::Different;
      if (aliasLength) aliasMatch = HalFileName::compare(wanted, std::string_view(alias.data(), aliasLength));
      if (aliasMatch == FileNameComparison::Invalid) return error("invalid alias");
      if (match == FileNameComparison::Equal || aliasMatch == FileNameComparison::Equal) {
        if (entry.isDirectory()) return error("directory collision");
        const uint64_t size = entry.fileSize64();
        if (!close() || (progress && !progress(context))) return error("presence close/cancel");
        output = size;
        return FileStatus::Present;
      }
      if (++steps == 32) {
        steps = 0;
        vTaskDelay(1);
      }
    }
  }

 private:
  HalFile directory, entry;
  std::array<char, INVENTORY_PATH_LIMIT + 1> parent{}, name{};
  std::array<char, 13> alias{};
  Progress progress;
  void* context;
  bool closeEntry() { return !entry.isOpen() || entry.close() || failure("entry close"); }
  bool close() {
    const bool entryClosed = closeEntry();
    const bool directoryClosed = !directory.isOpen() || directory.close();
    if (!directoryClosed) failure("directory close");
    return entryClosed && directoryClosed;
  }
  static bool failure(const char* operation) {
    LOG_ERR("COMPANION", "Inventory path lookup %s failed", operation);
    return false;
  }
  FileStatus error(const char* operation) {
    failure(operation);
    close();
    return FileStatus::Error;
  }
};
}  // namespace companion
