#pragma once

#include <cstdint>
#include <span>

namespace companion {
struct ZipPathDetails {
  uint16_t rawBytes = 0, trimmedBytes = 0;
  bool trailingSlash = false;
  bool operator==(const ZipPathDetails&) const = default;
};
// Streaming UTF-8 path grammar. Directory trimming is byte-exact; Unicode
// normalization and archive-wide duplicate/case ambiguity checks are separate.
class ZipPathValidation final {
 public:
  static constexpr uint16_t MAX_BYTES = 1024;
  void reset() {
    bytes = componentBytes = 0;
    scalar = minimum = 0;
    remaining = 0;
    dotsOnly = true;
    slash = failed = finished = false;
  }
  bool consume(std::span<const uint8_t> input) {
    if (failed || finished) return failure();
    for (const auto byte : input) {
      if (bytes == MAX_BYTES) return failure();
      ++bytes;
      if (!remaining && byte == '/') {
        if (!component()) return failure();
        componentBytes = 0;
        dotsOnly = true;
        slash = true;
        continue;
      }
      if (byte < 32 || byte == 127 || byte == '\\' || byte == ':' || !utf8(byte)) return failure();
      ++componentBytes;
      dotsOnly = dotsOnly && byte == '.';
      slash = false;
    }
    return true;
  }
  bool finish(bool directory, ZipPathDetails& output) {
    if (failed || finished || !bytes || remaining || (slash ? !directory : !component())) return failure();
    output = {bytes, static_cast<uint16_t>(bytes - (slash ? 1 : 0)), slash};
    finished = true;
    return true;
  }

 private:
  uint32_t scalar = 0, minimum = 0;
  uint16_t bytes = 0, componentBytes = 0;
  uint8_t remaining = 0;
  bool dotsOnly = true, slash = false, failed = false, finished = false;
  bool component() const { return componentBytes && !(dotsOnly && componentBytes <= 2); }
  bool failure() {
    failed = true;
    return false;
  }
  bool utf8(uint8_t byte) {
    if (remaining) {
      if ((byte & 0xc0) != 0x80) return false;
      scalar = (scalar << 6) | (byte & 0x3f);
      return --remaining != 0 || (scalar >= minimum && scalar <= 0x10ffff && (scalar < 0xd800 || scalar > 0xdfff));
    }
    if (byte <= 0x7f) return true;
    if (byte >= 0xc2 && byte <= 0xdf) {
      remaining = 1;
      scalar = byte & 0x1f;
      minimum = 0x80;
    } else if (byte >= 0xe0 && byte <= 0xef) {
      remaining = 2;
      scalar = byte & 0xf;
      minimum = 0x800;
    } else if (byte >= 0xf0 && byte <= 0xf4) {
      remaining = 3;
      scalar = byte & 7;
      minimum = 0x10000;
    } else
      return false;
    return true;
  }
};
}  // namespace companion
