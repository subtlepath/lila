#pragma once

#include "CompanionJournalMergeIntent.h"
#include "HalCompanionFileLookup.h"

namespace companion {
enum class JournalMergeRecord { Intent, Receipt, Receiving, Aborting };
// Session-owned buffers exceed the task stack. The caller excludes publication writers.
class HalJournalMergeRecordStore {
 public:
  static constexpr const char* INTENT = "/.crosspoint/companion/journal-merge";
  static constexpr const char* INTENT_STAGE = "/.crosspoint/companion/journal-merge-next";
  static constexpr const char* RECEIPT = "/.crosspoint/companion/journal-merge-receipt";
  static constexpr const char* RECEIPT_STAGE = "/.crosspoint/companion/journal-merge-receipt-next";
  static constexpr const char* RECEIVING = "/.crosspoint/companion/journal-merge-receiving";
  static constexpr const char* RECEIVING_STAGE = "/.crosspoint/companion/journal-merge-receiving-next";
  static constexpr const char* ABORTING = "/.crosspoint/companion/journal-merge-aborting";
  static constexpr const char* ABORTING_STAGE = "/.crosspoint/companion/journal-merge-aborting-next";
  explicit HalJournalMergeRecordStore(JournalMergeRecord record = JournalMergeRecord::Intent)
      : path(record == JournalMergeRecord::Aborting    ? ABORTING
             : record == JournalMergeRecord::Receipt   ? RECEIPT
             : record == JournalMergeRecord::Receiving ? RECEIVING
                                                       : INTENT),
        stage(record == JournalMergeRecord::Aborting    ? ABORTING_STAGE
              : record == JournalMergeRecord::Receipt   ? RECEIPT_STAGE
              : record == JournalMergeRecord::Receiving ? RECEIVING_STAGE
                                                        : INTENT_STAGE),
        replace(record == JournalMergeRecord::Receipt) {}
  // Borrowed paths must outlive the store; used for immutable per-transaction abort receipts.
  HalJournalMergeRecordStore(const char* recordPath, const char* stagePath, bool replaceRecords = false)
      : path(recordPath), stage(stagePath), replace(replaceRecords) {}
  ~HalJournalMergeRecordStore() {
    if (file.isOpen() && !file.close()) failure("destructor close");
  }
  JournalMigrationPresence load(JournalMergeIntent& output) { return read(path, output); }
  bool persist(const JournalMergeIntent& expected) {
    if (!encodeJournalMergeIntent(expected, encoded)) return failure("invalid record");
    const auto presence = load(prior);
    if (presence == JournalMigrationPresence::IoError) return failure("existing record");
    if (presence == JournalMigrationPresence::Present && prior == expected) return true;
    if (presence == JournalMigrationPresence::Present && !replace) return failure("intent conflict");
    if (!Storage.openFileForWriteReusing("COMPANION", stage, file) || file.isDirectory()) return failure("stage open");
    const bool written = file.seek64(0) && file.write(encoded.data(), encoded.size()) == encoded.size() &&
                         file.truncate(encoded.size()) && file.sync();
    const bool closed = file.close();
    if (!written || !closed) return failure("stage write/sync/close");
    if (read(stage, current) != JournalMigrationPresence::Present || current != expected)
      return failure("stage readback");
    const auto before = load(current);
    if (before == JournalMigrationPresence::IoError) return failure("destination lookup");
    if (before == JournalMigrationPresence::Present) {
      if (current == expected) return true;
      if (!replace || presence != JournalMigrationPresence::Present || current != prior)
        return failure("replacement conflict");
      if (!Storage.remove(path)) return failure("receipt remove");
    } else if (presence != JournalMigrationPresence::Missing) {
      return failure("destination changed");
    }
    if (!Storage.rename(stage, path)) return failure("record rename");
    return (load(current) == JournalMigrationPresence::Present && current == expected) || failure("record readback");
  }
  bool clear(const JournalMergeIntent& expected) {
    const auto presence = load(current);
    if (presence == JournalMigrationPresence::Missing) return true;
    if (presence != JournalMigrationPresence::Present || current != expected) return failure("clear conflict");
    return Storage.remove(path) || failure("record remove");
  }

 private:
  JournalMigrationPresence read(const char* source, JournalMergeIntent& output) {
    if ((file.isOpen() && !file.close()) || !Storage.ensureDirectoryExists(TRANSFER_DIRECTORY))
      return error("prepare read");
    const auto presence = lookup.inspect(source);
    if (presence == CompanionFilePresence::Missing) return JournalMigrationPresence::Missing;
    if (presence == CompanionFilePresence::Error || !Storage.openFileForReadReusing("COMPANION", source, file))
      return error("lookup/open");
    const bool loaded = !file.isDirectory() && file.fileSize64() == scratch.size() &&
                        file.read(scratch.data(), scratch.size()) == static_cast<int>(scratch.size()) &&
                        file.fileSize64() == scratch.size();
    const bool closed = file.close();
    if (!loaded || !closed || !decodeJournalMergeIntent(scratch, decoded)) return error("read/format/close");
    output = decoded;
    return JournalMigrationPresence::Present;
  }
  static bool failure(const char* reason) {
    LOG_ERR("COMPANION", "Journal merge record failed: %s", reason);
    return false;
  }
  static JournalMigrationPresence error(const char* reason) {
    failure(reason);
    return JournalMigrationPresence::IoError;
  }
  const char* path;
  const char* stage;
  bool replace;
  HalCompanionFileLookup lookup;
  HalFile file;
  std::array<uint8_t, JOURNAL_MERGE_INTENT_SIZE> scratch{}, encoded{};
  JournalMergeIntent current, prior, decoded;
};
}  // namespace companion
