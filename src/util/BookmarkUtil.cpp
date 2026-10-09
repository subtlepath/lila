#include "BookmarkUtil.h"

#include <algorithm>
#include <cctype>
#include <string>

std::string BookmarkUtil::getBookmarksDir() { return "/.crosspoint/bookmarks/"; }

std::string BookmarkUtil::getBookmarkPath(const std::string& bookPath) {
  // remove leading slash and replace internal slashes to create a flat filename
  std::string bookName = std::string(bookPath).erase(0, 1);
  std::replace(bookName.begin(), bookName.end(), '/', '_');
  std::replace(bookName.begin(), bookName.end(), '\\', '_');
  const size_t lastDot = bookName.find_last_of('.');
  if (lastDot != std::string::npos) {
    bookName.erase(lastDot);
  }
  bookName += ".json";
  return getBookmarksDir() + bookName;
}

bool BookmarkUtil::getBookmarkPath(std::string_view bookPath, std::span<char> output) {
  static constexpr std::string_view PREFIX = "/.crosspoint/bookmarks/";
  static constexpr std::string_view SUFFIX = ".json";
  static constexpr size_t MAX_PATH_SIZE = 511;
  if (bookPath.size() < 2 || bookPath.size() > MAX_PATH_SIZE || bookPath.front() != '/' ||
      bookPath.find('\0') != std::string_view::npos)
    return false;
  bookPath.remove_prefix(1);
  const auto dot = bookPath.find_last_of('.');
  if (dot != std::string_view::npos) bookPath = bookPath.substr(0, dot);
  const size_t size = PREFIX.size() + bookPath.size() + SUFFIX.size();
  if (size > MAX_PATH_SIZE || output.size() <= size) return false;
  std::copy(PREFIX.begin(), PREFIX.end(), output.begin());
  size_t at = PREFIX.size();
  for (const char byte : bookPath) output[at++] = byte == '/' || byte == '\\' ? '_' : byte;
  std::copy(SUFFIX.begin(), SUFFIX.end(), output.begin() + at);
  output[size] = 0;
  return true;
}

bool BookmarkUtil::getBookmarkEditionPath(std::span<const uint8_t> edition, std::span<char> output) {
  static constexpr std::string_view PREFIX = "/.crosspoint/bookmarks/editions/";
  static constexpr std::string_view SUFFIX = ".json";
  static constexpr char DIGITS[] = "0123456789abcdef";
  static constexpr size_t EDITION_BYTES = 32;
  static constexpr size_t SIZE = PREFIX.size() + EDITION_BYTES * 2 + SUFFIX.size();
  if (edition.size() != EDITION_BYTES || output.size() <= SIZE ||
      std::all_of(edition.begin(), edition.end(), [](uint8_t byte) { return byte == 0; }))
    return false;
  std::copy(PREFIX.begin(), PREFIX.end(), output.begin());
  size_t at = PREFIX.size();
  for (const auto byte : edition) {
    output[at++] = DIGITS[byte >> 4];
    output[at++] = DIGITS[byte & 15];
  }
  std::copy(SUFFIX.begin(), SUFFIX.end(), output.begin() + at);
  output[SIZE] = 0;
  return true;
}

std::string BookmarkUtil::sanitizeBookmarkSummary(std::string summary) {
  summary.erase(std::unique(summary.begin(), summary.end(),
                            [](unsigned char a, unsigned char b) { return std::isspace(a) && std::isspace(b); }),
                summary.end());
  summary.erase(std::remove(summary.begin(), summary.end(), '\n'), summary.end());
  summary.erase(summary.begin(),
                std::find_if(summary.begin(), summary.end(), [](unsigned char ch) { return !std::isspace(ch); }));
  summary.erase(
      std::find_if(summary.rbegin(), summary.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(),
      summary.end());
  static constexpr size_t MAX_SUMMARY_BYTES = 72;
  if (summary.size() > MAX_SUMMARY_BYTES) {
    size_t end = MAX_SUMMARY_BYTES;
    while (end && (static_cast<unsigned char>(summary[end]) & 0xc0) == 0x80) --end;
    summary.resize(end);
  }
  return summary;
}
