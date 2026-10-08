#pragma once

#include "CompanionPreferenceBody.h"

namespace companion {
struct ReadingAnchor {
  uint16_t spine = 0;
  uint32_t visibleTextOffset = 0;
  bool operator==(const ReadingAnchor&) const = default;
};
// Text views borrow the body and are not null-terminated.
struct BookmarkBodyView {
  Identity identity{};
  bool deleted = false;
  ReadingAnchor anchor{};
  std::span<const uint8_t> name, summary;
};
inline constexpr size_t MAX_BOOKMARK_BODY_SIZE = 668;
namespace reading_body_detail {
inline uint32_t number(std::span<const uint8_t> bytes, size_t at, unsigned count) {
  uint32_t value = 0;
  for (unsigned i = 0; i < count; ++i) value |= static_cast<uint32_t>(bytes[at + i]) << (8 * i);
  return value;
}
inline ReadingAnchor anchor(std::span<const uint8_t> bytes, size_t at) {
  return {static_cast<uint16_t>(number(bytes, at, 2)), number(bytes, at + 2, 4)};
}
inline bool text(std::span<const uint8_t> bytes) {
  for (const auto byte : bytes)
    if (!byte) return false;
  return validPreferenceUtf8(bytes);
}
}  // namespace reading_body_detail
inline bool decodeReadingAnchor(std::span<const uint8_t> bytes, ReadingAnchor& output) {
  if (bytes.size() != 8 || bytes[0] != 1 || bytes[1] != static_cast<uint8_t>(EventKind::ReadingPosition)) return false;
  output = reading_body_detail::anchor(bytes, 2);
  return true;
}
inline bool decodeBookmarkBody(std::span<const uint8_t> bytes, BookmarkBodyView& output) {
  if (bytes.size() < 18 || bytes.size() > MAX_BOOKMARK_BODY_SIZE || bytes[0] != 1 ||
      (bytes[1] != static_cast<uint8_t>(EventKind::BookmarkPut) &&
       bytes[1] != static_cast<uint8_t>(EventKind::BookmarkDelete)))
    return false;
  BookmarkBodyView value;
  bool nonzero = false;
  for (unsigned i = 0; i < value.identity.size(); ++i) {
    value.identity[i] = bytes[2 + i];
    nonzero |= value.identity[i] != 0;
  }
  if (!nonzero) return false;
  value.deleted = bytes[1] == static_cast<uint8_t>(EventKind::BookmarkDelete);
  if (value.deleted) {
    if (bytes.size() != 18) return false;
  } else {
    if (bytes.size() < 28) return false;
    value.anchor = reading_body_detail::anchor(bytes, 18);
    const auto nameLength = reading_body_detail::number(bytes, 24, 2);
    if (nameLength > 128 || bytes.size() < 28 + nameLength) return false;
    value.name = bytes.subspan(26, nameLength);
    const auto summaryLength = reading_body_detail::number(bytes, 26 + nameLength, 2);
    if (summaryLength > 512 || bytes.size() != 28 + nameLength + summaryLength) return false;
    value.summary = bytes.subspan(28 + nameLength, summaryLength);
    if (!reading_body_detail::text(value.name) || !reading_body_detail::text(value.summary)) return false;
  }
  output = value;
  return true;
}
// Text spans must not overlap output. Invalid input leaves output untouched.
inline size_t encodeBookmarkBody(const BookmarkBodyView& bookmark, std::span<uint8_t> output) {
  if (std::all_of(bookmark.identity.begin(), bookmark.identity.end(), [](uint8_t byte) { return byte == 0; })) return 0;
  const size_t size = bookmark.deleted ? 18 : 28 + bookmark.name.size() + bookmark.summary.size();
  if (!bookmark.deleted && (bookmark.name.size() > 128 || bookmark.summary.size() > 512 ||
                            !reading_body_detail::text(bookmark.name) || !reading_body_detail::text(bookmark.summary)))
    return 0;
  if (output.size() < size) return 0;
  output[0] = 1;
  output[1] = static_cast<uint8_t>(bookmark.deleted ? EventKind::BookmarkDelete : EventKind::BookmarkPut);
  std::copy(bookmark.identity.begin(), bookmark.identity.end(), output.begin() + 2);
  if (bookmark.deleted) return size;
  for (unsigned i = 0; i < 2; ++i) output[18 + i] = static_cast<uint8_t>(bookmark.anchor.spine >> (8 * i));
  for (unsigned i = 0; i < 4; ++i) output[20 + i] = static_cast<uint8_t>(bookmark.anchor.visibleTextOffset >> (8 * i));
  for (unsigned i = 0; i < 2; ++i) output[24 + i] = static_cast<uint8_t>(bookmark.name.size() >> (8 * i));
  std::copy(bookmark.name.begin(), bookmark.name.end(), output.begin() + 26);
  const auto summaryAt = 26 + bookmark.name.size();
  for (unsigned i = 0; i < 2; ++i) output[summaryAt + i] = static_cast<uint8_t>(bookmark.summary.size() >> (8 * i));
  std::copy(bookmark.summary.begin(), bookmark.summary.end(), output.begin() + summaryAt + 2);
  return size;
}
}  // namespace companion
