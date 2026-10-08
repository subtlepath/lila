#pragma once

#include "CompanionInventoryIndex.h"

namespace companion {
// The caller supplies central-directory values and the checked end of payload.
// Source ownership must prevent mutation throughout archive validation.
class ZipDescriptorValidation final {
 public:
  using Progress = bool (*)(void*);
  ZipDescriptorValidation(InventoryIndexStorage& source, std::span<uint8_t> scratch, Progress progress = nullptr,
                          void* context = nullptr)
      : source(source), scratch(scratch), progress(progress), context(context) {}
  bool validate(uint64_t offset, uint64_t limit, bool zip64, uint32_t crc, uint64_t compressedBytes,
                uint64_t expandedBytes, uint64_t& outputEnd) {
    uint64_t length = 0;
    const size_t base = zip64 ? 20 : 12;
    if (scratch.size() < 24 || !source.size(length) || limit > length || offset > limit || base > limit - offset ||
        (!zip64 && (compressedBytes > UINT32_MAX || expandedBytes > UINT32_MAX)) || !tick() ||
        !source.read(offset, scratch.first(base)))
      return false;
    const bool unsignedMatch = matches(0, zip64, crc, compressedBytes, expandedBytes);
    bool signedMatch = false;
    if (inventory_detail::read(scratch, 0, 4) == 0x08074b50 && base + 4 <= limit - offset) {
      if (!tick() || !source.read(offset, scratch.first(base + 4))) return false;
      signedMatch = matches(4, zip64, crc, compressedBytes, expandedBytes);
    }
    // Two matching interpretations have different ownership ranges. Reject
    // ambiguity instead of consuming bytes belonging to a following record.
    uint64_t finalLength = 0;
    if (unsignedMatch == signedMatch || !source.size(finalLength) || finalLength != length || !tick()) return false;
    outputEnd = offset + base + (signedMatch ? 4 : 0);
    return true;
  }

 private:
  InventoryIndexStorage& source;
  std::span<uint8_t> scratch;
  Progress progress;
  void* context;
  bool tick() const { return !progress || progress(context); }
  bool matches(size_t at, bool zip64, uint32_t crc, uint64_t compressed, uint64_t expanded) const {
    const unsigned width = zip64 ? 8 : 4;
    return inventory_detail::read(scratch, at, 4) == crc &&
           inventory_detail::read(scratch, at + 4, width) == compressed &&
           inventory_detail::read(scratch, at + 4 + width, width) == expanded;
  }
};
}  // namespace companion
