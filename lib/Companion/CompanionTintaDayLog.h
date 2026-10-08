#pragma once

#include <array>
#include <span>

#include "../Serialization/BinaryRecordBytes.h"

namespace companion {
// Validates canonical companion snapshots one record at a time.
class TintaDayLogValidator {
 public:
  bool begin(std::span<const uint8_t> header, uint32_t fileSize) {
    *this = TintaDayLogValidator{};
    if (header.size() != 4 || fileSize < 4 || (fileSize - 4) % 12 != 0 || header[0] != 'T' || header[1] != 'D' ||
        header[2] != 'L' || header[3] != '1')
      return false;
    remaining = (fileSize - 4) / 12;
    valid = true;
    return true;
  }

  bool record(std::span<const uint8_t> bytes) {
    if (!valid) return false;
    if (!remaining || bytes.size() != 12 ||
        binary_record::getU16(bytes.data() + 10) != static_cast<uint16_t>(binary_record::crc32(bytes.data(), 10)))
      return fail();
    const uint16_t nextDay = binary_record::getU16(bytes.data());
    if (hasDay && nextDay < day) return fail();
    if (!hasDay || nextDay != day) {
      if (hasDay && !consistent()) return fail();
      totals.fill(0);
      day = nextDay;
      hasDay = true;
    }
    for (size_t i = 0; i < totals.size(); ++i) {
      const uint32_t value = binary_record::getU16(bytes.data() + 2 + i * 2);
      if (totals[i] > UINT32_MAX - value) return fail();
      totals[i] += value;
    }
    --remaining;
    return true;
  }

  bool finish() {
    if (!valid || remaining || (hasDay && !consistent())) return fail();
    return true;
  }

 private:
  bool consistent() const { return totals[1] <= totals[0] && totals[2] <= totals[0]; }
  bool fail() {
    valid = false;
    return false;
  }
  std::array<uint32_t, 4> totals{};
  uint32_t remaining = 0;
  uint16_t day = 0;
  bool hasDay = false;
  bool valid = false;
};
}  // namespace companion
