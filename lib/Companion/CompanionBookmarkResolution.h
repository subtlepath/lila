#pragma once

#include "CompanionJournalReplayOrder.h"
#include "CompanionReadingBody.h"

namespace companion {
// Retain off-stack; caller freezes journal/index and owns disposable visit marks.
// Output is a canonical body, including explicit deletion, and changes only on Ok.
class BookmarkResolution final {
 public:
  using Visitor = bool (*)(void*, const EventIdentity&, std::span<const uint8_t>);
  TintaJournalResult run(TintaJournal& journal, JournalIdentityIndex& index, JournalReplayVisits& marks,
                         const Digest& edition, const Identity& bookmark, std::span<uint8_t> output, size_t& length) {
    return scan(journal, index, marks, edition, bookmark, output, length, nullptr, nullptr);
  }
  // Each body is borrowed only during the callback. Consumers discard partial
  // results unless Ok and recheck authority before showing or applying choices.
  TintaJournalResult visitHeads(TintaJournal& journal, JournalIdentityIndex& index, JournalReplayVisits& marks,
                                const Digest& edition, const Identity& bookmark, Visitor visitor, void* context) {
    if (!visitor) return TintaJournalResult::Invalid;
    size_t ignored = 0;
    return scan(journal, index, marks, edition, bookmark, selected, ignored, visitor, context);
  }

 private:
  TintaJournalResult scan(TintaJournal& journal, JournalIdentityIndex& index, JournalReplayVisits& marks,
                          const Digest& edition, const Identity& bookmark, std::span<uint8_t> output, size_t& length,
                          Visitor visitor, void* context) {
    if (!journal.available()) return TintaJournalResult::Unavailable;
    if (!tinta_body_detail::nonzero(edition) || !tinta_body_detail::nonzero(bookmark) ||
        output.size() < MAX_BOOKMARK_BODY_SIZE)
      return TintaJournalResult::Invalid;
    const auto count = journal.count();
    const auto validated = validation.validate(journal, index);
    if (validated != TintaJournalResult::Ok) return validated;
    if (!marks.reset(count)) return TintaJournalResult::IoError;
    size_t selectedLength = 0;
    bool conflict = false;
    for (uint32_t remaining = count; remaining; --remaining) {
      const auto record = remaining - 1;
      bool marked = false;
      if (!marks.visited(record, marked)) return TintaJournalResult::IoError;
      const auto read = journal.read(record);
      if (read != TintaJournalResult::Ok) return read;
      event = journal.event();
      bool matching = false;
      if ((event.kind == EventKind::BookmarkPut || event.kind == EventKind::BookmarkDelete) &&
          event.resource == edition) {
        BookmarkBodyView value;
        if (!decodeBookmarkBody(journal.body(), value)) return TintaJournalResult::Corrupt;
        matching = value.identity == bookmark;
      }
      if (matching && !marked) {
        const auto body = journal.body();
        if (visitor && !visitor(context, event.identity, body)) return TintaJournalResult::IoError;
        if (!selectedLength) {
          selectedLength = body.size();
          std::copy(body.begin(), body.end(), selected.begin());
        } else if (body.size() != selectedLength || !std::equal(body.begin(), body.end(), selected.begin())) {
          conflict = true;
        }
      }
      if (matching || marked) {
        for (unsigned at = 0; at < event.ancestorCount; ++at) {
          const auto result = markParent(index, marks, event.ancestors[at], record);
          if (result != TintaJournalResult::Ok) return result;
        }
        if (event.identity.sequence > 1) {
          predecessor = event.identity;
          --predecessor.sequence;
          const auto result = markParent(index, marks, predecessor, record);
          if (result != TintaJournalResult::Ok) return result;
        }
      }
      if (journal.count() != count) return TintaJournalResult::Conflict;
    }
    if (conflict && !visitor) return TintaJournalResult::Conflict;
    if (!selectedLength) return TintaJournalResult::Unavailable;
    if (!visitor) {
      std::copy_n(selected.begin(), selectedLength, output.begin());
      length = selectedLength;
    }
    return TintaJournalResult::Ok;
  }

  static TintaJournalResult markParent(JournalIdentityIndex& index, JournalReplayVisits& marks,
                                       const EventIdentity& identity, uint32_t child) {
    uint32_t record = 0;
    const auto found = index.find(identity, record);
    if (found == JournalIdentityLookup::IoError) return TintaJournalResult::IoError;
    if (found != JournalIdentityLookup::Found || record >= child) return TintaJournalResult::Corrupt;
    return marks.mark(record) ? TintaJournalResult::Ok : TintaJournalResult::IoError;
  }
  JournalCausalValidation validation;
  SyncEvent event;
  EventIdentity predecessor;
  std::array<uint8_t, MAX_BOOKMARK_BODY_SIZE> selected{};
};
}  // namespace companion
