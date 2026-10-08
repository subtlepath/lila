#pragma once

#include "CompanionInventoryIndex.h"

namespace companion {
struct ZipExtraValues {
  uint64_t expandedBytes = 0, compressedBytes = 0, localOffset = 0;
  uint32_t disk = 0;
  bool zip64 = false;
  bool operator==(const ZipExtraValues&) const = default;
};
// Header values are supplied before expanding sentinel fields. The source must
// remain immutable under the session owner's exclusive access.
class ZipExtraValidation final {
 public:
  using Progress = bool (*)(void*);
  ZipExtraValidation(InventoryIndexStorage& source, std::span<uint8_t> scratch, Progress progress = nullptr,
                     void* context = nullptr)
      : source(source), scratch(scratch), progress(progress), context(context) {}
  bool validate(uint64_t offset, uint16_t bytes, bool local, const ZipExtraValues& header, ZipExtraValues& output) {
    uint64_t length = 0;
    if (scratch.size() < 28 || !source.size(length) || offset > length || bytes > length - offset ||
        header.expandedBytes > UINT32_MAX || header.compressedBytes > UINT32_MAX || header.localOffset > UINT32_MAX ||
        header.disk > UINT16_MAX)
      return false;
    ZipExtraValues parsed = header;
    parsed.zip64 = false;
    const bool expanded = header.expandedBytes == UINT32_MAX, compressed = header.compressedBytes == UINT32_MAX;
    const bool location = !local && header.localOffset == UINT32_MAX, disk = !local && header.disk == UINT16_MAX;
    const bool required = expanded || compressed || location || disk;
    const uint64_t end = offset + bytes;
    while (offset < end) {
      if (end - offset < 4 || !read(offset, 4)) return false;
      const auto id = number(0, 2), count = number(2, 2);
      offset += 4;
      if (count > end - offset) return false;
      if (id == 1) {
        if (parsed.zip64) return false;
        parsed.zip64 = true;
        const size_t expected =
            local ? 16 : (expanded ? 8 : 0) + (compressed ? 8 : 0) + (location ? 8 : 0) + (disk ? 4 : 0);
        if (count != expected || (count && !read(offset, expected))) return false;
        size_t at = 0;
        if (local || expanded) {
          parsed.expandedBytes = number(at, 8);
          at += 8;
        }
        if (local || compressed) {
          parsed.compressedBytes = number(at, 8);
          at += 8;
        }
        if (location) {
          parsed.localOffset = number(at, 8);
          at += 8;
        }
        if (disk) parsed.disk = static_cast<uint32_t>(number(at, 4));
        // Non-sentinel local fields must agree with their extended values.
        if (local && ((!expanded && parsed.expandedBytes != header.expandedBytes) ||
                      (!compressed && parsed.compressedBytes != header.compressedBytes)))
          return false;
      }
      offset += count;
    }
    uint64_t finalLength = 0;
    if ((required && !parsed.zip64) || !source.size(finalLength) || finalLength != length || !tick()) return false;
    output = parsed;
    return true;
  }

 private:
  InventoryIndexStorage& source;
  std::span<uint8_t> scratch;
  Progress progress;
  void* context;
  bool tick() const { return !progress || progress(context); }
  bool read(uint64_t offset, size_t count) { return tick() && source.read(offset, scratch.first(count)); }
  uint64_t number(size_t offset, unsigned width) const { return inventory_detail::read(scratch, offset, width); }
};
}  // namespace companion
