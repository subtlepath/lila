#pragma once

#include <algorithm>

#include "../Serialization/BinaryRecordBytes.h"
#include "CompanionRecords.h"

namespace companion {
enum class TintaDerivedFile : uint8_t { Items, LocalReviews, Lessons, Readings, Days };
inline constexpr size_t TINTA_DERIVED_MANIFEST_SIZE = 332;

// Borrowed immutable bytes must outlive the view. No file content is verified here.
class TintaDerivedManifestView {
 public:
  bool decode(std::span<const uint8_t> input) {
    bytes = {};
    if (input.size() != TINTA_DERIVED_MANIFEST_SIZE || input[0] != 'T' || input[1] != 'D' || input[2] != 'S' ||
        input[3] != '1' || binary_record::getU32(input.data() + 328) != binary_record::crc32(input.data(), 328) ||
        input[118] || input[119] || read(input, 120, 8) == 0)
      return false;
    for (const auto part : {input.subspan(4, 16), input.subspan(20, 32), input.subspan(52, 32), input.subspan(84, 16),
                            input.subspan(100, 16)})
      if (std::none_of(part.begin(), part.end(), [](uint8_t byte) { return byte != 0; })) return false;
    for (size_t i = 0; i < 5; ++i) {
      const uint64_t length = read(input, 128 + i * 40, 8);
      const auto hash = input.subspan(136 + i * 40, 32);
      if (std::none_of(hash.begin(), hash.end(), [](uint8_t byte) { return byte != 0; })) return false;
      if (i == 0 && (length < 1024 || length > 1024 + 32767 * 16 || (length - 1024) % 16)) return false;
      if (i == 1 && length != 0) return false;
      if ((i == 2 || i == 3) && (length < 16 || length > 16 + 65535 * 4 || (length - 16) % 4)) return false;
      if (i == 4 && (length < 4 || length > UINT32_MAX || (length - 4) % 12)) return false;
    }
    bytes = input;
    return true;
  }
  bool matches(const Identity& course, const Identity& storage, const Digest& pack, const Digest& frontier) const {
    return !bytes.empty() && std::equal(course.begin(), course.end(), bytes.begin() + 4) &&
           std::equal(storage.begin(), storage.end(), bytes.begin() + 100) &&
           std::equal(pack.begin(), pack.end(), bytes.begin() + 20) &&
           std::equal(frontier.begin(), frontier.end(), bytes.begin() + 52);
  }
  uint64_t revision() const { return bytes.empty() ? 0 : read(bytes, 120, 8); }
  bool matchesBytes(std::span<const uint8_t> input) const {
    return !bytes.empty() && input.size() == bytes.size() && std::equal(bytes.begin(), bytes.end(), input.begin());
  }
  uint16_t studyDay() const { return bytes.empty() ? 0 : binary_record::getU16(bytes.data() + 116); }
  std::span<const uint8_t> snapshotIdentity() const { return bytes.empty() ? bytes : bytes.subspan(84, 16); }
  uint64_t length(TintaDerivedFile file) const {
    const auto index = static_cast<size_t>(file);
    return bytes.empty() || index >= 5 ? 0 : read(bytes, 128 + index * 40, 8);
  }
  std::span<const uint8_t> hash(TintaDerivedFile file) const {
    const auto index = static_cast<size_t>(file);
    return bytes.empty() || index >= 5 ? std::span<const uint8_t>{} : bytes.subspan(136 + index * 40, 32);
  }

 private:
  static uint64_t read(std::span<const uint8_t> input, size_t offset, size_t count) {
    uint64_t value = 0;
    for (size_t i = 0; i < count; ++i) value |= static_cast<uint64_t>(input[offset + i]) << (i * 8);
    return value;
  }
  std::span<const uint8_t> bytes;
};
}  // namespace companion
