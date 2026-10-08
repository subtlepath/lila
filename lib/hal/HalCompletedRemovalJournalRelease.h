#pragma once

#include "HalCompletedContentRemovals.h"
#include "HalContentRemovalJournalStorage.h"

namespace companion {
// Release only journal files bound to a synced immutable completion receipt.
// A surviving older slot can be released after an interrupted first deletion.
// The caller excludes all journal writers throughout this session-owned operation.
class HalCompletedRemovalJournalRelease final {
 public:
  HalCompletedRemovalJournalRelease(ContentRemovalJournal& journal, HalContentRemovalJournalStorage& storage,
                                    HalCompletedContentRemovals& completions, std::span<uint8_t> scratch)
      : journal(journal), storage(storage), completions(completions), scratch(scratch) {}
  CompletedRemovalResult release() {
    if (!journal.current() || scratch.size() < CONTENT_REMOVAL_RECORD_SIZE) return CompletedRemovalResult::Invalid;
    checkpoint = *journal.current();
    const auto result = completions.load(checkpoint.request, receipt);
    if (result != CompletedRemovalResult::Ok) return result;
    if (!guard() || receipt.planHash != checkpoint.planHash || receipt.request != checkpoint.request)
      return CompletedRemovalResult::Conflict;
    // Check both files before removing either, including every valid older slot.
    for (size_t i = 0; i < 2; ++i) {
      const auto checked = inspect(i);
      if (checked != CompletedRemovalResult::Ok) return checked;
      present[i] = found;
      records[i] = decoded;
    }
    for (size_t i = 0; i < 2; ++i) {
      if (!present[i]) continue;
      const auto checked = inspect(i);
      if (checked != CompletedRemovalResult::Ok || !found || decoded != records[i] || !guard()) {
        journal.invalidate();
        return checked == CompletedRemovalResult::Ok ? CompletedRemovalResult::Conflict : checked;
      }
      if (!Storage.remove(CONTENT_REMOVAL_JOURNALS[i]) || !guard()) {
        journal.invalidate();
        return error("journal removal");
      }
      uint64_t length = 0;
      const auto absent = storage.stat(CONTENT_REMOVAL_JOURNALS[i], length);
      if (!guard() || absent != FileStatus::Missing) {
        journal.invalidate();
        return error("journal absence");
      }
    }
    for (const auto* path : CONTENT_REMOVAL_JOURNALS) {
      uint64_t length = 0;
      if (!guard() || storage.stat(path, length) != FileStatus::Missing || !guard()) {
        journal.invalidate();
        return error("final absence");
      }
    }
    journal.invalidate();
    return CompletedRemovalResult::Ok;
  }

 private:
  ContentRemovalJournal& journal;
  HalContentRemovalJournalStorage& storage;
  HalCompletedContentRemovals& completions;
  std::span<uint8_t> scratch;
  ContentRemovalRecord checkpoint, receipt, decoded;
  std::array<ContentRemovalRecord, 2> records;
  std::array<bool, 2> present{};
  bool found = false;
  bool guard() const { return journal.current() && *journal.current() == checkpoint; }
  CompletedRemovalResult inspect(size_t i) {
    if (!guard()) return CompletedRemovalResult::Conflict;
    uint64_t size = 0;
    const auto status = storage.stat(CONTENT_REMOVAL_JOURNALS[i], size);
    if (!guard()) return CompletedRemovalResult::Conflict;
    found = status == FileStatus::Present;
    if (status == FileStatus::Missing) return CompletedRemovalResult::Ok;
    if (status != FileStatus::Present) return error("slot lookup");
    if (size != CONTENT_REMOVAL_RECORD_SIZE) return CompletedRemovalResult::Corrupt;
    if (!storage.read(CONTENT_REMOVAL_JOURNALS[i], scratch.first(CONTENT_REMOVAL_RECORD_SIZE)))
      return error("slot read");
    if (!guard()) return CompletedRemovalResult::Conflict;
    if (!decodeContentRemovalRecord(scratch.first(CONTENT_REMOVAL_RECORD_SIZE), decoded))
      return CompletedRemovalResult::Corrupt;
    if (decoded.request != receipt.request || decoded.planHash != receipt.planHash)
      return CompletedRemovalResult::Conflict;
    return CompletedRemovalResult::Ok;
  }
  static CompletedRemovalResult error(const char* reason) {
    LOG_ERR("COMPANION", "Completed removal journal release failed: %s", reason);
    return CompletedRemovalResult::IoError;
  }
};
}  // namespace companion
