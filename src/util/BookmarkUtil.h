#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

class BookmarkUtil {
 public:
  static std::string getBookmarksDir();
  static std::string getBookmarkPath(const std::string& bookPath);
  static bool getBookmarkPath(std::string_view bookPath, std::span<char> output);
  static bool getBookmarkEditionPath(std::span<const uint8_t> edition, std::span<char> output);
  static std::string sanitizeBookmarkSummary(std::string summary);
};
