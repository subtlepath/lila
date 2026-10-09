#pragma once

#include <string_view>
#include <vector>

#include "CompanionBookmarkReaderBinding.h"

class Epub;
struct BookmarkEntry;

namespace companion {
class NativeBookmarkChoicePage;
// Caller freezes book/path and rebuilds row bindings after every mutable list load.
TintaJournalResult restoreReaderBookmarks(Epub& epub, std::vector<BookmarkEntry>& entries,
                                          ReaderBookmarkBinding& binding);
TintaJournalResult decideReaderLegacyBookmarks(Epub& epub, std::vector<BookmarkEntry>& entries,
                                               ReaderBookmarkBinding& binding, LegacyBookmarkDecision decision);
TintaJournalResult createReaderBookmark(Epub& epub, BookmarkEntry& candidate, std::vector<BookmarkEntry>& entries,
                                        ReaderBookmarkBinding& binding);
TintaJournalResult renameReaderBookmark(Epub& epub, const BookmarkEntry& selected, std::string_view name,
                                        std::vector<BookmarkEntry>& entries, ReaderBookmarkBinding& binding);
TintaJournalResult deleteReaderBookmark(Epub& epub, const BookmarkEntry& selected, std::vector<BookmarkEntry>& entries,
                                        ReaderBookmarkBinding& binding);
using ReaderBookmarkMatcher = bool (*)(void*, const BookmarkEntry&);
TintaJournalResult deleteMatchingReaderBookmarks(Epub& epub, std::vector<BookmarkEntry>& entries,
                                                 ReaderBookmarkBinding& binding, ReaderBookmarkMatcher matches,
                                                 void* matchContext);
TintaJournalResult loadReaderBookmarkChoices(Epub& epub, NativeBookmarkChoicePage& choices,
                                             ReaderBookmarkBinding& binding, uint32_t offset = 0);
TintaJournalResult resolveReaderBookmarkChoice(Epub& epub, NativeBookmarkChoicePage& choices, uint32_t selected,
                                               std::vector<BookmarkEntry>& entries, ReaderBookmarkBinding& binding);
}  // namespace companion
