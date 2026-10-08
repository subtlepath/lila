#pragma once

#include <Logging.h>

#include <cstring>

#include "CompanionDictionaryExtractionJournal.h"
#include "CompanionDictionaryInstallationJournal.h"
#include "CompanionDictionaryRetirementJournal.h"
#include "CompanionDictionaryZipAudit.h"
#include "HalCompanionFileLookup.h"

namespace companion {
// Session-owned; one record handle and two checked-lookup handles are retained.
// The parent exclusively owns the selected journal slots throughout recovery.
class HalDictionaryExtractionJournalStorage final : public DictionaryExtractionJournalStorage {
 public:
  enum class Purpose { Extraction, Installation, Retirement, ZipAudit };
  explicit HalDictionaryExtractionJournalStorage(Purpose purpose = Purpose::Extraction) : purpose(purpose) {}
  bool prepare() override {
    return (recordBytes() && Storage.ready() && Storage.ensureDirectoryExists(TRANSFER_DIRECTORY)) ||
           failure("prepare");
  }
  FileStatus stat(const char* path, uint64_t& length) override {
    if (!allowed(path) || !close()) return FileStatus::Error;
    const auto presence = lookup.inspect(path);
    if (presence == CompanionFilePresence::Missing) return FileStatus::Missing;
    if (presence != CompanionFilePresence::Present || !Storage.openFileForReadReusing("COMPANION", path, file) ||
        file.isDirectory()) {
      failure("stat");
      close();
      return FileStatus::Error;
    }
    const auto bytes = file.fileSize64();
    if (!close()) return FileStatus::Error;
    length = bytes;
    return FileStatus::Present;
  }
  bool read(const char* path, uint64_t at, std::span<uint8_t> bytes) override {
    if (!allowed(path) || !close() || bytes.size() != recordBytes() || at != 0 ||
        !Storage.openFileForReadReusing("COMPANION", path, file))
      return failure("read open");
    const bool ok = !file.isDirectory() && file.fileSize64() == bytes.size() && file.seek64(0) &&
                    file.read(bytes.data(), bytes.size()) == static_cast<int>(bytes.size());
    const bool closed = close();
    return (ok && closed) || failure("read");
  }
  bool write(const char* path, uint64_t at, std::span<const uint8_t> bytes, bool truncate) override {
    if (!allowed(path) || at != 0 || !truncate || bytes.size() != recordBytes() || !close())
      return failure("write arguments");
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
  const Purpose purpose;
  size_t recordBytes() const {
    return purpose == Purpose::Extraction     ? DICTIONARY_EXTRACTION_RECEIPT_SIZE
           : purpose == Purpose::Installation ? DICTIONARY_INSTALLATION_PLAN_SIZE
           : purpose == Purpose::Retirement   ? DICTIONARY_RETIREMENT_PROOF_SIZE
           : purpose == Purpose::ZipAudit     ? DICTIONARY_ZIP_AUDIT_SIZE
                                              : 0;
  }
  bool close() { return !file.isOpen() || file.close() || failure("close"); }
  bool allowed(const char* path) const {
    if (!path || !recordBytes()) return false;
    const auto& paths = purpose == Purpose::Extraction     ? DICTIONARY_EXTRACTION_JOURNALS
                        : purpose == Purpose::Installation ? DICTIONARY_INSTALLATION_JOURNALS
                        : purpose == Purpose::Retirement   ? DICTIONARY_RETIREMENT_JOURNALS
                                                           : DICTIONARY_ZIP_AUDIT_JOURNALS;
    return std::strcmp(path, paths[0]) == 0 || std::strcmp(path, paths[1]) == 0;
  }
  static bool failure(const char* operation) {
    LOG_ERR("COMPANION", "Dictionary journal %s failed", operation);
    return false;
  }
};
}  // namespace companion
