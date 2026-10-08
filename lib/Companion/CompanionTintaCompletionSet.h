#pragma once

#include <span>

#include "../Serialization/BinaryRecordBytes.h"

namespace companion {
enum class TintaCompletionKind : uint8_t { Lessons = 1, Readings = 2 };

class TintaCompletionSetValidator {
 public:
  bool begin(std::span<const uint8_t> header, uint32_t fileSize, TintaCompletionKind expected) {
    *this = TintaCompletionSetValidator{};
    if (header.size() != 12 || header[0] != 'T' || header[1] != 'C' || header[2] != 'S' || header[3] != '1' ||
        (expected != TintaCompletionKind::Lessons && expected != TintaCompletionKind::Readings) ||
        header[4] != static_cast<uint8_t>(expected) || header[5] || header[6] || header[7])
      return false;
    remaining = binary_record::getU32(header.data() + 8);
    if (remaining > UINT16_MAX || fileSize != 16 + remaining * 4) return false;
    crc = binary_record::crc32(header.data(), header.size());
    valid = true;
    return true;
  }
  bool identity(std::span<const uint8_t> bytes) {
    if (!valid || !remaining || bytes.size() != 4) return fail();
    const uint32_t value = binary_record::getU32(bytes.data());
    if (value <= previous || value == UINT32_MAX) return fail();
    previous = value;
    crc = binary_record::crc32Update(crc, bytes.data(), bytes.size());
    --remaining;
    return true;
  }
  bool finish(std::span<const uint8_t> checksum) {
    if (!valid || remaining || checksum.size() != 4 || binary_record::getU32(checksum.data()) != crc) return fail();
    return true;
  }

 private:
  bool fail() {
    valid = false;
    return false;
  }
  uint32_t remaining = 0;
  uint32_t previous = 0;
  uint32_t crc = 0;
  bool valid = false;
};
}  // namespace companion
