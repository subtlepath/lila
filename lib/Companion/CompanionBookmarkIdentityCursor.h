#pragma once

#include "CompanionTintaJournal.h"

namespace companion {
enum class BookmarkCursorResult : uint8_t { Pending, Found, End, Error };

// Caller audits and freezes authority through enumeration. Fixed memory; each
// distinct ID requires a journal scan, split into bounded steps so callers yield.
class BookmarkIdentityCursor final {
 public:
  static constexpr uint32_t RECORDS_PER_STEP = 64;
  bool begin(const TintaJournal& journal, const Digest& verifiedEdition) {
    *this = {};
    if (!journal.available()) return failBegin(TintaJournalResult::Unavailable);
    if (!tinta_body_detail::nonzero(verifiedEdition)) return failBegin(TintaJournalResult::Invalid);
    edition = verifiedEdition;
    count = journal.count();
    recordSize = journal.recordSize();
    lastError = TintaJournalResult::Ok;
    ready = true;
    return true;
  }
  BookmarkCursorResult step(TintaJournal& journal, Identity& output) {
    if (!ready) return BookmarkCursorResult::Error;
    if (!journal.available()) return fail(TintaJournalResult::Unavailable);
    if (journal.count() != count || journal.recordSize() != recordSize) return fail(TintaJournalResult::Conflict);
    if (ended) return BookmarkCursorResult::End;
    uint32_t processed = 0;
    while (record < count && processed < RECORDS_PER_STEP) {
      const auto read = journal.read(record);
      if (read != TintaJournalResult::Ok) return fail(read);
      const auto& event = journal.event();
      if ((event.kind == EventKind::BookmarkPut || event.kind == EventKind::BookmarkDelete) &&
          event.resource == edition) {
        BookmarkBodyView bookmark;
        if (!decodeBookmarkBody(journal.body(), bookmark)) return fail(TintaJournalResult::Corrupt);
        if ((!hasPrevious || previous < bookmark.identity) && (!selected || bookmark.identity < candidate)) {
          candidate = bookmark.identity;
          selected = true;
        }
      }
      ++record;
      ++processed;
    }
    if (journal.count() != count || journal.recordSize() != recordSize) return fail(TintaJournalResult::Conflict);
    if (record < count) return BookmarkCursorResult::Pending;
    if (!selected) {
      ended = true;
      return BookmarkCursorResult::End;
    }
    output = candidate;
    previous = candidate;
    hasPrevious = true;
    selected = false;
    record = 0;
    return BookmarkCursorResult::Found;
  }
  TintaJournalResult error() const { return lastError; }

 private:
  bool failBegin(TintaJournalResult result) {
    lastError = result;
    return false;
  }
  BookmarkCursorResult fail(TintaJournalResult result) {
    ready = false;
    lastError = result;
    return BookmarkCursorResult::Error;
  }
  Digest edition{};
  Identity previous{}, candidate{};
  uint32_t count = 0, record = 0;
  uint16_t recordSize = 0;
  TintaJournalResult lastError = TintaJournalResult::Unavailable;
  bool ready = false, ended = false, selected = false, hasPrevious = false;
};
}  // namespace companion
