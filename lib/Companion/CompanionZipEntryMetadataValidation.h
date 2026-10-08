#pragma once
#include "CompanionZipDescriptorValidation.h"
#include "CompanionZipDirectoryLayout.h"
#include "CompanionZipEntryExtraction.h"
#include "CompanionZipExtraValidation.h"

namespace companion {
struct ZipEntryMetadata {
  ZipEntrySpan payload;
  uint64_t nextCentralOffset = 0, nameOffset = 0, localOffset = 0, localEnd = 0;
  uint16_t nameBytes = 0;
  bool directory = false, zip64 = false;
};
// Session-owned working state; no filename allocation. Names are exposed as raw
// archive spans for subsequent decoding and archive-wide path validation.
class ZipEntryMetadataValidation final {
 public:
  using Progress = bool (*)(void*);
  ZipEntryMetadataValidation(InventoryIndexStorage& source, std::span<uint8_t> scratch, Progress progress = nullptr,
                             void* context = nullptr)
      : source(source),
        scratch(scratch),
        progress(progress),
        context(context),
        extras(source, scratch, progress, context),
        descriptors(source, scratch, progress, context) {}
  bool validate(const ZipDirectoryLayout& layout, uint64_t central, ZipEntryMetadata& output) {
    uint64_t length = 0;
    if (scratch.size() < 64 || !source.size(length) || length != layout.archiveBytes ||
        layout.centralOffset > layout.recordsOffset || layout.recordsOffset > length ||
        layout.centralBytes > layout.recordsOffset - layout.centralOffset || central < layout.centralOffset ||
        central > layout.centralOffset + layout.centralBytes ||
        layout.centralOffset + layout.centralBytes - central < 46 || !read(central, 46) || number(0, 4) != 0x02014b50)
      return false;
    parsed = {};
    needed = number(6, 2);
    parsed.payload.flags = number(8, 2);
    parsed.payload.method = number(10, 2);
    parsed.payload.crc = number(16, 4);
    values = {number(24, 4), number(20, 4), number(42, 4), static_cast<uint32_t>(number(34, 2))};
    parsed.nameBytes = number(28, 2);
    extraBytes = number(30, 2);
    const uint64_t variable = parsed.nameBytes + extraBytes + number(32, 2);
    const auto host = number(4, 2) >> 8, attributes = number(38, 4), mode = attributes >> 16;
    parsed.directory = (attributes & 0x10) != 0;
    if (host == 3 || host == 19) {
      const auto type = mode & 0170000;
      if (type && type != 0100000 && type != 0040000) return false;
      if (type == 0040000) parsed.directory = true;
    }
    if (!parsed.nameBytes || parsed.nameBytes > 1024 ||
        variable > layout.centralOffset + layout.centralBytes - central - 46 ||
        (parsed.payload.method != 0 && parsed.payload.method != 8) ||
        (parsed.payload.flags & ~(parsed.payload.method == 8 ? 0x80e : 0x808)))
      return false;
    parsed.nameOffset = central + 46;
    parsed.nextCentralOffset = central + 46 + variable;
    if (!read(parsed.nameOffset + parsed.nameBytes - 1, 1)) return false;
    parsed.directory = parsed.directory || scratch[0] == '/';
    if (!extras.validate(parsed.nameOffset + parsed.nameBytes, extraBytes, false, values, values) || values.disk)
      return false;
    parsed.payload.expandedBytes = values.expandedBytes;
    parsed.payload.compressedBytes = values.compressedBytes;
    parsed.localOffset = values.localOffset;
    parsed.zip64 = values.zip64;
    if (values.expandedBytes > ZipEntryExtraction::MAX_EXPANDED_BYTES || (parsed.directory && values.expandedBytes) ||
        (parsed.payload.method == 0 && values.expandedBytes != values.compressedBytes) ||
        parsed.localOffset > layout.centralOffset || layout.centralOffset - parsed.localOffset < 30 ||
        !read(parsed.localOffset, 30) || number(0, 4) != 0x04034b50 || number(4, 2) != needed ||
        number(6, 2) != parsed.payload.flags || number(8, 2) != parsed.payload.method ||
        number(26, 2) != parsed.nameBytes)
      return false;
    localCrc = number(14, 4);
    values = {number(22, 4), number(18, 4), 0, 0};
    extraBytes = number(28, 2);
    const auto localName = parsed.localOffset + 30;
    if (parsed.nameBytes + extraBytes > layout.centralOffset - localName || !namesEqual(localName)) return false;
    parsed.payload.offset = localName + parsed.nameBytes + extraBytes;
    if (!extras.validate(localName + parsed.nameBytes, extraBytes, true, values, values)) return false;
    parsed.zip64 = parsed.zip64 || values.zip64;
    if (needed < (parsed.zip64 ? 45 : parsed.payload.method == 8 ? 20 : 10)) return false;
    const bool streaming = parsed.payload.flags & 8;
    if ((localCrc != parsed.payload.crc && (!streaming || localCrc)) ||
        (values.expandedBytes != parsed.payload.expandedBytes && (!streaming || values.expandedBytes)) ||
        (values.compressedBytes != parsed.payload.compressedBytes && (!streaming || values.compressedBytes)) ||
        parsed.payload.compressedBytes > layout.centralOffset - parsed.payload.offset)
      return false;
    parsed.localEnd = parsed.payload.offset + parsed.payload.compressedBytes;
    if (streaming &&
        !descriptors.validate(parsed.localEnd, layout.centralOffset, parsed.zip64, parsed.payload.crc,
                              parsed.payload.compressedBytes, parsed.payload.expandedBytes, parsed.localEnd))
      return false;
    uint64_t finalLength = 0;
    if (!source.size(finalLength) || finalLength != length || !tick()) return false;
    output = parsed;
    return true;
  }

 private:
  InventoryIndexStorage& source;
  std::span<uint8_t> scratch;
  Progress progress;
  void* context;
  ZipExtraValidation extras;
  ZipDescriptorValidation descriptors;
  ZipEntryMetadata parsed;
  ZipExtraValues values;
  uint32_t localCrc = 0;
  uint16_t needed = 0, extraBytes = 0;
  bool tick() const { return !progress || progress(context); }
  bool read(uint64_t offset, size_t count) { return tick() && source.read(offset, scratch.first(count)); }
  uint64_t number(size_t at, unsigned width) const { return inventory_detail::read(scratch, at, width); }
  bool namesEqual(uint64_t local) {
    const auto half = scratch.size() / 2;
    for (size_t at = 0; at < parsed.nameBytes;) {
      const auto count = std::min<size_t>(half, parsed.nameBytes - at);
      if (!tick() || !source.read(local + at, scratch.first(count)) ||
          !source.read(parsed.nameOffset + at, scratch.subspan(half, count)) ||
          !std::equal(scratch.begin(), scratch.begin() + count, scratch.begin() + half))
        return false;
      at += count;
    }
    return true;
  }
};
}  // namespace companion
