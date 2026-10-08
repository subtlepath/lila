#pragma once

#include "CompanionInventoryIndex.h"

namespace companion {
struct ZipDirectoryLayout {
  uint64_t archiveBytes = 0, centralOffset = 0, centralBytes = 0, entries = 0, recordsOffset = 0;
  bool zip64 = false;
  bool operator==(const ZipDirectoryLayout&) const = default;
};
// Borrowed source/scratch; scans the complete permitted ZIP comment tail in
// overlapping banks. This validates end metadata, not entries or member bytes.
class ZipDirectoryLayoutValidation final {
 public:
  using Progress = bool (*)(void*);
  ZipDirectoryLayoutValidation(InventoryIndexStorage& source, std::span<uint8_t> scratch, uint64_t maxEntries = 20000,
                               Progress progress = nullptr, void* context = nullptr)
      : source(source), scratch(scratch), maxEntries(maxEntries), progress(progress), context(context) {}
  bool validate(ZipDirectoryLayout& output) {
    uint64_t length = 0;
    if (scratch.size() < 64 || !maxEntries || !source.size(length) || length < 22) return false;
    const uint64_t lower = length > 65557 ? length - 65557 : 0;
    for (uint64_t end = length;;) {
      const uint64_t start = end - std::min<uint64_t>(scratch.size(), end - lower);
      const auto count = static_cast<size_t>(end - start);
      if (!read(start, count)) return false;
      for (size_t at = count - 22 + 1; at-- > 0;) {
        const auto header = std::span<const uint8_t>(scratch).subspan(at, 22);
        if (number(header, 0, 4) != 0x06054b50 || number(header, 20, 2) != length - (start + at) - 22) continue;
        ZipDirectoryLayout parsed;
        parsed.archiveBytes = length;
        parsed.recordsOffset = start + at;
        if (!parse(header, parsed) || parsed.entries > maxEntries || parsed.centralOffset > parsed.recordsOffset ||
            parsed.centralBytes > parsed.recordsOffset - parsed.centralOffset ||
            parsed.entries > parsed.centralBytes / 46)
          return false;
        uint64_t finalBytes = 0;
        if (!source.size(finalBytes) || finalBytes != length || !tick()) return false;
        output = parsed;
        return true;
      }
      if (start == lower) return false;
      end = start + 21;
    }
  }

 private:
  InventoryIndexStorage& source;
  std::span<uint8_t> scratch;
  uint64_t maxEntries;
  Progress progress;
  void* context;
  static uint64_t number(std::span<const uint8_t> bytes, size_t offset, unsigned width) {
    return inventory_detail::read(bytes, offset, width);
  }
  bool tick() const { return !progress || progress(context); }
  bool read(uint64_t offset, size_t bytes) { return tick() && source.read(offset, scratch.first(bytes)); }
  bool parse(std::span<const uint8_t> header, ZipDirectoryLayout& parsed) {
    if (number(header, 4, 2) || number(header, 6, 2)) return false;
    const uint64_t diskEntries = number(header, 8, 2), entries = number(header, 10, 2);
    const uint64_t centralBytes = number(header, 12, 4), centralOffset = number(header, 16, 4);
    const bool required =
        diskEntries == 65535 || entries == 65535 || centralBytes == UINT32_MAX || centralOffset == UINT32_MAX;
    const auto endOffset = parsed.recordsOffset;
    bool locator = false;
    if (endOffset >= 20) {
      if (!read(endOffset - 20, 20)) return false;
      locator = number(scratch, 0, 4) == 0x07064b50;
    }
    if (!locator) {
      if (required || diskEntries != entries) return false;
      parsed.entries = entries;
      parsed.centralBytes = centralBytes;
      parsed.centralOffset = centralOffset;
      return true;
    }
    if (number(scratch, 4, 4) != 0 || number(scratch, 16, 4) != 1) return false;
    const auto recordOffset = number(scratch, 8, 8), locatorOffset = endOffset - 20;
    if (recordOffset > locatorOffset || locatorOffset - recordOffset < 56 || !read(recordOffset, 56)) return false;
    if (number(scratch, 0, 4) != 0x06064b50 || number(scratch, 4, 8) < 44 ||
        number(scratch, 4, 8) != locatorOffset - recordOffset - 12 || number(scratch, 14, 2) < 45 ||
        number(scratch, 16, 4) || number(scratch, 20, 4) || number(scratch, 24, 8) != number(scratch, 32, 8))
      return false;
    parsed.entries = number(scratch, 32, 8);
    parsed.centralBytes = number(scratch, 40, 8);
    parsed.centralOffset = number(scratch, 48, 8);
    if ((diskEntries != 65535 && diskEntries != parsed.entries) || (entries != 65535 && entries != parsed.entries) ||
        (centralBytes != UINT32_MAX && centralBytes != parsed.centralBytes) ||
        (centralOffset != UINT32_MAX && centralOffset != parsed.centralOffset))
      return false;
    parsed.recordsOffset = recordOffset;
    parsed.zip64 = true;
    return true;
  }
};
}  // namespace companion
