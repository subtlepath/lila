#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace companion {
// Reader callback must enforce partition bounds and return exact reads.
using FirmwareImageRead = bool (*)(void*, uint64_t, std::span<uint8_t>);
inline bool firmwareImageExtent(FirmwareImageRead read, void* context, uint64_t capacity, uint64_t& output) {
  if (!read || capacity < 24) return false;
  std::array<uint8_t, 24> header;
  if (!read(context, 0, header) || header[0] != 0xe9 || header[1] == 0 || header[1] > 16 || header[23] > 1)
    return false;
  uint64_t position = header.size();
  std::array<uint8_t, 8> segment;
  for (unsigned index = 0; index < header[1]; ++index) {
    if (capacity - position < segment.size() || !read(context, position, segment)) return false;
    position += segment.size();
    uint32_t length = 0;
    for (size_t byte = 0; byte < 4; ++byte) length |= uint32_t(segment[4 + byte]) << (8 * byte);
    if (length > capacity - position) return false;
    position += length;
  }
  // A checksum byte always follows the segments, padded to a 16-byte boundary.
  const uint64_t padding = 16 - (position & 15);
  const uint64_t trailer = header[23] ? 32 : 0;
  if (padding > capacity - position || trailer > capacity - position - padding) return false;
  output = position + padding + trailer;
  return true;
}
}  // namespace companion
