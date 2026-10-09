#pragma once
#include <string>
#include <vector>

#include "../BookmarkEntry.h"

// Per-book bookmark persistence. Takes the book's path; bookmark-file path
// derivation (BookmarkUtil) and directory creation are hidden inside.
namespace BookmarkFile {

// Loads the bookmarks for bookPath. The vector is cleared first; a missing or
// empty bookmark file yields an empty list and returns false.
bool load(const std::string& bookPath, std::vector<BookmarkEntry>& bookmarks);

// Reads an explicit stored file (for verified migration backups). Preserves the
// stored name length so migration can reject unsupported values without truncation.
bool loadFromPath(const char* path, std::vector<BookmarkEntry>& bookmarks, bool strict = true);

// Saves the bookmarks for bookPath, creating the bookmarks directory as needed.
bool save(const std::string& bookPath, const std::vector<BookmarkEntry>& bookmarks);

}  // namespace BookmarkFile
