#pragma once

#include <string_view>

#include "CompanionReadingBody.h"

namespace companion {
class BookmarkJsonSink {
 public:
  virtual ~BookmarkJsonSink() = default;
  virtual bool write(std::span<const uint8_t> bytes) = 0;
};
// Retain off-stack. Sink owns staging; no output may be published until finish
// and the source spool's verified End both succeed.
class BookmarkJsonWriter final {
 public:
  explicit BookmarkJsonWriter(BookmarkJsonSink& sink) : sink(sink) {}
  bool begin() {
    if (started) return false;
    started = true;
    return emit("{\"bookmarks\":[");
  }
  bool append(std::span<const uint8_t> body) {
    if (!started || finished || failed) return false;
    BookmarkBodyView bookmark;
    if (!decodeBookmarkBody(body, bookmark) || (hasPrevious && !(previous < bookmark.identity))) return fail();
    if (!bookmark.deleted) {
      if ((emitted && !put(',')) || !emit("{\"id\":\"")) return false;
      for (const auto byte : bookmark.identity)
        if (!put(HEX_DIGITS[byte >> 4]) || !put(HEX_DIGITS[byte & 15])) return false;
      if (!emit("\",\"si\":") || !number(bookmark.anchor.spine) || !emit(",\"vo\":") ||
          !number(bookmark.anchor.visibleTextOffset) || !emit(",\"name\":") || !text(bookmark.name) ||
          !emit(",\"summary\":") || !text(bookmark.summary) || !put('}'))
        return false;
      emitted = true;
    }
    previous = bookmark.identity;
    hasPrevious = true;
    return true;
  }
  bool finish() {
    if (!started || finished || failed || !emit("]}") || !flush()) return false;
    finished = true;
    return true;
  }
  uint64_t bytesWritten() const { return written; }

 private:
  static constexpr char HEX_DIGITS[] = "0123456789abcdef";
  bool fail() {
    failed = true;
    return false;
  }
  bool flush() {
    if (failed) return false;
    if (!used) return true;
    if (written > UINT64_MAX - used || !sink.write(std::span(buffer).first(used))) return fail();
    written += used;
    used = 0;
    return true;
  }
  bool put(uint8_t byte) {
    if (failed || (used == buffer.size() && !flush())) return false;
    buffer[used++] = byte;
    return true;
  }
  bool emit(std::string_view value) {
    for (const auto byte : value)
      if (!put(static_cast<uint8_t>(byte))) return false;
    return true;
  }
  bool number(uint32_t value) {
    std::array<char, 10> digits{};
    size_t first = digits.size();
    do {
      digits[--first] = static_cast<char>('0' + value % 10);
      value /= 10;
    } while (value);
    return emit(std::string_view(digits.data() + first, digits.size() - first));
  }
  bool text(std::span<const uint8_t> value) {
    if (!put('"')) return false;
    for (const auto byte : value) {
      if (byte == '"' || byte == '\\') {
        if (!put('\\') || !put(byte)) return false;
      } else if (byte < 32) {
        if (!emit("\\u00") || !put(HEX_DIGITS[byte >> 4]) || !put(HEX_DIGITS[byte & 15])) return false;
      } else if (!put(byte)) {
        return false;
      }
    }
    return put('"');
  }
  BookmarkJsonSink& sink;
  std::array<uint8_t, 128> buffer{};
  Identity previous{};
  uint64_t written = 0;
  size_t used = 0;
  bool started = false, finished = false, failed = false, emitted = false, hasPrevious = false;
};
}  // namespace companion
