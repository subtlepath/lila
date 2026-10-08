#pragma once

#include <Logging.h>

#include <cstring>

#include "CompanionContentRemovalJournal.h"
#include "HalCompanionFileLookup.h"

namespace companion {
// Session-owned outside the task stack. HAL wrappers are prepared once and reused.
class HalContentRemovalJournalStorage final : public ContentRemovalJournalStorage {
 public:
  bool prepare() override {
    return (Storage.ready() && Storage.ensureDirectoryExists(TRANSFER_DIRECTORY)) || failure("prepare");
  }
  FileStatus stat(const char* path, uint64_t& size) override {
    if (!allowed(path) || !close()) {
      failure("stat arguments");
      return FileStatus::Error;
    }
    const auto presence = lookup.inspect(path);
    if (presence == CompanionFilePresence::Missing) return FileStatus::Missing;
    if (presence != CompanionFilePresence::Present || !Storage.openFileForReadReusing("COMPANION", path, file) ||
        file.isDirectory()) {
      failure("stat");
      close();
      return FileStatus::Error;
    }
    const auto length = file.fileSize64();
    if (!close()) return FileStatus::Error;
    size = length;
    return FileStatus::Present;
  }
  bool read(const char* path, std::span<uint8_t> bytes) override {
    if (!allowed(path) || bytes.size() != CONTENT_REMOVAL_RECORD_SIZE || !close() ||
        !Storage.openFileForReadReusing("COMPANION", path, file))
      return failure("read open");
    const bool ok = !file.isDirectory() && file.fileSize64() == bytes.size() && file.seek64(0) &&
                    file.read(bytes.data(), bytes.size()) == static_cast<int>(bytes.size());
    const bool closed = close();
    return (ok && closed) || failure("read");
  }
  bool write(const char* path, std::span<const uint8_t> bytes) override {
    if (!allowed(path) || bytes.size() != CONTENT_REMOVAL_RECORD_SIZE || !close()) return failure("write arguments");
    uint64_t length = 0;
    if (stat(path, length) == FileStatus::Error || !Storage.openFileForWriteReusing("COMPANION", path, file))
      return failure("write open");
    const bool ok = !file.isDirectory() && file.write(bytes.data(), bytes.size()) == bytes.size() &&
                    file.truncate(bytes.size()) && file.sync() && file.fileSize64() == bytes.size();
    const bool closed = close();
    return (ok && closed) || failure("write/sync");
  }

 private:
  HalFile file;
  HalCompanionFileLookup lookup;
  bool close() { return !file.isOpen() || file.close() || failure("close"); }
  static bool allowed(const char* path) {
    return path &&
           (std::strcmp(path, CONTENT_REMOVAL_JOURNALS[0]) == 0 || std::strcmp(path, CONTENT_REMOVAL_JOURNALS[1]) == 0);
  }
  static bool failure(const char* operation) {
    LOG_ERR("COMPANION", "Removal journal %s failed", operation);
    return false;
  }
};
}  // namespace companion
