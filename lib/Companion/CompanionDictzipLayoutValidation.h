#pragma once

#include "CompanionInventoryIndex.h"

namespace companion {
struct DictzipLayout {
  uint64_t dataOffset = 0, tableOffset = 0, compressedBytes = 0;
  uint32_t expandedBytes = 0, crc = 0;
  uint16_t chunkLength = 0, chunks = 0;
  bool operator==(const DictzipLayout&) const = default;
};
// Structural checks only: decoded chunk sizes, DEFLATE termination and CRC
// must also be verified before accepting an asset. No chunk table is allocated.
class DictzipLayoutValidation final {
 public:
  using Progress = bool (*)(void*);
  DictzipLayoutValidation(InventoryIndexStorage& source, std::span<uint8_t> scratch, Progress progress = nullptr,
                          void* context = nullptr)
      : source(source), scratch(scratch), progress(progress), context(context) {}
  bool validate(DictzipLayout& output) {
    if (scratch.size() < 12 || !source.size(length) || length < 20 || !read(0, 12)) return false;
    if (scratch[0] != 31 || scratch[1] != 139 || scratch[2] != 8 || !(scratch[3] & 4) || (scratch[3] & 0xe0))
      return false;
    const uint8_t flags = scratch[3];
    const uint64_t extraEnd = 12 + number(10, 2);
    if (extraEnd > length - 8) return false;
    DictzipLayout parsed;
    bool found = false;
    for (uint64_t at = 12; at < extraEnd;) {
      if (extraEnd - at < 4 || !read(at, 4)) return false;
      const bool ra = scratch[0] == 'R' && scratch[1] == 'A';
      const auto size = number(2, 2);
      if (size > extraEnd - at - 4) return false;
      if (ra) {
        if (found || size < 6 || !read(at, 10) || number(4, 2) != 1) return false;
        parsed.chunkLength = number(6, 2);
        parsed.chunks = number(8, 2);
        if (!parsed.chunkLength || !parsed.chunks || parsed.chunks > 8192 || size != 6 + uint32_t(parsed.chunks) * 2)
          return false;
        parsed.tableOffset = at + 10;
        found = true;
      }
      at += 4 + size;
    }
    if (!found || !read(length - 8, 8)) return false;
    parsed.crc = number(0, 4);
    parsed.expandedBytes = number(4, 4);
    if (!parsed.expandedBytes || (uint64_t(parsed.expandedBytes) - 1) / parsed.chunkLength + 1 != parsed.chunks)
      return false;
    uint64_t position = extraEnd;
    for (const uint8_t mask : {uint8_t{8}, uint8_t{16}}) {
      if (!(flags & mask)) continue;
      bool terminated = false;
      for (unsigned index = 0; index < 65536; ++index) {
        if (position >= length - 8 || !read(position++, 1)) return false;
        if (!scratch[0]) {
          terminated = true;
          break;
        }
      }
      if (!terminated) return false;
    }
    if (flags & 2) position += 2;
    if (position > length - 8) return false;
    parsed.dataOffset = position;
    for (unsigned index = 0; index < parsed.chunks; ++index) {
      if (!read(parsed.tableOffset + uint64_t(index) * 2, 2)) return false;
      const auto size = number(0, 2);
      if (size <= 4 || size > length - 8 - position || !read(position + size - 4, 4) || scratch[0] != 0 ||
          scratch[1] != 0 || scratch[2] != 255 || scratch[3] != 255)
        return false;
      position += size;
    }
    if (position >= length - 8) return false;
    parsed.compressedBytes = position - parsed.dataOffset;
    output = parsed;
    return true;
  }

 private:
  InventoryIndexStorage& source;
  std::span<uint8_t> scratch;
  Progress progress;
  void* context;
  uint64_t length = 0;
  bool read(uint64_t at, size_t count) {
    return (!progress || progress(context)) && at <= length && count <= length - at &&
           source.read(at, scratch.first(count));
  }
  uint32_t number(unsigned at, unsigned count) const {
    uint32_t value = 0;
    for (unsigned byte = 0; byte < count; ++byte) value |= uint32_t(scratch[at + byte]) << (byte * 8);
    return value;
  }
};
}  // namespace companion
