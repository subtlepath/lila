#pragma once

#include "CompanionJournalMigrationIntent.h"
#include "HalCompanionFileLookup.h"

namespace companion {
// Session-owned workspace; caller excludes writers and has verified both journals.
class HalJournalMigrationIntentStore {
 public:
  static constexpr const char* PATH = "/.crosspoint/companion/journal-migration";
  static constexpr const char* STAGE = "/.crosspoint/companion/journal-migration-next";
  ~HalJournalMigrationIntentStore() {
    if (file.isOpen() && !file.close()) failure("destructor close");
  }
  JournalMigrationPresence load(JournalMigrationIntent& output) { return read(PATH, output); }
  bool persist(const JournalMigrationIntent& intent) {
    if (!encodeJournalMigrationIntent(intent, expected)) return failure("invalid intent");
    JournalMigrationIntent current;
    const auto presence = load(current);
    if (presence == JournalMigrationPresence::Present) return matches(current, intent) || failure("intent conflict");
    if (presence != JournalMigrationPresence::Missing) return failure("intent lookup");
    if (!Storage.openFileForWriteReusing("COMPANION", STAGE, file) || file.isDirectory()) return failure("stage open");
    const bool written = file.seek64(0) && file.write(expected.data(), expected.size()) == expected.size() &&
                         file.truncate(expected.size()) && file.sync();
    const bool closed = file.close();
    if (!written || !closed) return failure("stage write/sync/close");
    if (read(STAGE, current) != JournalMigrationPresence::Present || !matches(current, intent))
      return failure("stage readback");
    const auto before = load(current);
    if (before == JournalMigrationPresence::Present) return matches(current, intent) || failure("intent conflict");
    if (before != JournalMigrationPresence::Missing || !Storage.rename(STAGE, PATH)) return failure("intent rename");
    if (load(current) != JournalMigrationPresence::Present || !matches(current, intent))
      return failure("intent readback");
    return true;
  }
  bool clear(const JournalMigrationIntent& intent) {
    JournalMigrationIntent current;
    const auto presence = load(current);
    if (presence == JournalMigrationPresence::Missing) return true;
    if (presence != JournalMigrationPresence::Present || !matches(current, intent)) return failure("clear conflict");
    return Storage.remove(PATH) || failure("intent remove");
  }

 private:
  static bool matches(const JournalMigrationIntent& a, const JournalMigrationIntent& b) {
    return a.count == b.count && a.frontier == b.frontier;
  }
  JournalMigrationPresence read(const char* path, JournalMigrationIntent& output) {
    if ((file.isOpen() && !file.close()) || !Storage.ensureDirectoryExists(TRANSFER_DIRECTORY))
      return error("prepare read");
    const auto presence = lookup.inspect(path);
    if (presence == CompanionFilePresence::Missing) return JournalMigrationPresence::Missing;
    if (presence == CompanionFilePresence::Error || !Storage.openFileForReadReusing("COMPANION", path, file))
      return error("lookup/open");
    const bool validExtent = !file.isDirectory() && file.fileSize64() == scratch.size();
    const bool loaded = validExtent && file.read(scratch.data(), scratch.size()) == static_cast<int>(scratch.size()) &&
                        file.fileSize64() == scratch.size();
    const bool closed = file.close();
    JournalMigrationIntent decoded;
    if (!loaded || !closed || !decodeJournalMigrationIntent(scratch, decoded)) return error("read/format/close");
    output = decoded;
    return JournalMigrationPresence::Present;
  }
  static bool failure(const char* reason) {
    LOG_ERR("COMPANION", "Journal migration intent failed: %s", reason);
    return false;
  }
  static JournalMigrationPresence error(const char* reason) {
    failure(reason);
    return JournalMigrationPresence::IoError;
  }
  HalCompanionFileLookup lookup;
  HalFile file;
  std::array<uint8_t, JOURNAL_MIGRATION_INTENT_SIZE> scratch{}, expected{};
};
}  // namespace companion
