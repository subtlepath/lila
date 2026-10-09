#pragma once

#include "CompanionInventoryPaths.h"
#include "CompanionTintaBody.h"

namespace companion {
inline constexpr char BOOKMARK_CACHE_PARENT[] = "/.crosspoint/bookmarks";
inline constexpr char BOOKMARK_EDITION_CACHE_PARENT[] = "/.crosspoint/bookmarks/editions";

// Flat paths remain valid for recovery of legacy transactions.
inline const char* bookmarkCacheParent(std::string_view path, const Digest& edition) {
  static constexpr std::string_view PREFIX = "/.crosspoint/bookmarks/";
  static constexpr std::string_view EDITION_PREFIX = "/.crosspoint/bookmarks/editions/";
  static constexpr std::string_view SUFFIX = ".json";
  static constexpr char DIGITS[] = "0123456789abcdef";
  if (!validInventoryPath(path) || !path.starts_with(PREFIX) || path.size() <= PREFIX.size()) return nullptr;
  if (path.substr(PREFIX.size()).find('/') == std::string_view::npos) return BOOKMARK_CACHE_PARENT;
  if (!tinta_body_detail::nonzero(edition) || !path.starts_with(EDITION_PREFIX) ||
      path.size() != EDITION_PREFIX.size() + edition.size() * 2 + SUFFIX.size() || !path.ends_with(SUFFIX))
    return nullptr;
  size_t at = EDITION_PREFIX.size();
  for (const auto byte : edition) {
    if (path[at++] != DIGITS[byte >> 4] || path[at++] != DIGITS[byte & 15]) return nullptr;
  }
  return BOOKMARK_EDITION_CACHE_PARENT;
}
}  // namespace companion
