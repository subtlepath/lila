#pragma once

#include <climits>

#include "CompanionJournalIdentityIndexBuilder.h"
#include "HalCompanionFileLookup.h"
#include "HalJournalIdentityIndexStorage.h"

namespace companion {
// Session-owned retained handles; caller excludes index readers/builders and journal writers.
class HalJournalIdentityIndexSink final : public JournalIdentityIndexSink {
 public:
  HalJournalIdentityIndexSink(uint32_t expectedCount, std::span<uint8_t> scratch)
      : expectedCount(expectedCount), scratch(scratch) {}
  ~HalJournalIdentityIndexSink() override { abort(); }
  bool begin() override {
    if (!cleanup() || scratch.size() < JOURNAL_IDENTITY_ENTRY_SIZE ||
        expectedCount > UINT32_MAX / TintaJournal::RECORD_SIZE || !Storage.ensureDirectoryExists(TRANSFER_DIRECTORY))
      return failure("prepare");
    const auto active = lookup.inspect(ACTIVE);
    const auto backup = lookup.inspect(BACKUP);
    if (active == CompanionFilePresence::Error || backup == CompanionFilePresence::Error)
      return failure("recovery lookup");
    if (active == CompanionFilePresence::Missing && backup == CompanionFilePresence::Present) {
      if (!regular(BACKUP) || !Storage.rename(BACKUP, ACTIVE)) return failure("restore backup");
    } else if (active == CompanionFilePresence::Present) {
      if (!regular(ACTIVE) || !remove(BACKUP)) return failure("discard backup");
    }
    if (!remove(CANDIDATE) || !Storage.openFileForWriteReusing("COMPANION", CANDIDATE, file) || file.isDirectory())
      return failure("open candidate");
    owned = writing = true;
    return true;
  }
  bool write(uint64_t offset, std::span<const uint8_t> bytes) override {
    if (!writing || bytes.size() > INT_MAX || bytes.size() > UINT64_MAX - offset || !file.seek64(offset) ||
        file.write(bytes.data(), bytes.size()) != bytes.size())
      return failure("write");
    if (++operations == 32) {
      operations = 0;
      vTaskDelay(1);
    }
    return true;
  }
  bool finish(uint64_t bytes) override {
    if (!writing || bytes != JOURNAL_IDENTITY_HEADER_SIZE + uint64_t{expectedCount} * JOURNAL_IDENTITY_ENTRY_SIZE ||
        file.fileSize64() != bytes || !file.sync() || file.fileSize64() != bytes || !file.close())
      return failure("sync/close");
    writing = false;
    if (!reader.open(CANDIDATE)) return failure("validation open");
    IndexedJournalIdentities index(reader, scratch);
    const bool valid = index.open(expectedCount) && !reader.hadReadError();
    const bool closed = reader.close();
    if (!valid || !closed) return failure("validation");
    const auto active = lookup.inspect(ACTIVE);
    if (active == CompanionFilePresence::Error) return failure("active lookup");
    if (active == CompanionFilePresence::Present && !Storage.rename(ACTIVE, BACKUP)) return failure("backup rename");
    if (!Storage.rename(CANDIDATE, ACTIVE)) return failure("publish rename");
    owned = false;
    return true;
  }
  void abort() override {
    if (!cleanup()) failure("abort cleanup");
  }
  static constexpr const char* ACTIVE = HalJournalIdentityIndexStorage::PATH;
  static constexpr char CANDIDATE[] = "/.crosspoint/companion/journal-identities-next";
  static constexpr char BACKUP[] = "/.crosspoint/companion/journal-identities-old";

 private:
  bool regular(const char* path) {
    if (!reader.open(path)) return false;
    return reader.close();
  }
  bool remove(const char* path) {
    const auto presence = lookup.inspect(path);
    if (presence == CompanionFilePresence::Missing) return true;
    return presence == CompanionFilePresence::Present && regular(path) && Storage.remove(path);
  }
  bool cleanup() {
    writing = false;
    operations = 0;
    if ((file.isOpen() && !file.close()) || !reader.close()) return false;
    if (owned && !remove(CANDIDATE)) return false;
    owned = false;
    return true;
  }
  bool failure(const char* operation) {
    writing = false;
    LOG_ERR("COMPANION", "Journal index %s failed", operation);
    return false;
  }
  uint32_t expectedCount;
  std::span<uint8_t> scratch;
  HalFile file;
  HalJournalIdentityIndexStorage reader;
  HalCompanionFileLookup lookup;
  uint8_t operations = 0;
  bool owned = false, writing = false;
};
}  // namespace companion
