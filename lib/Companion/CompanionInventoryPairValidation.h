#pragma once

#include "CompanionInventoryPaths.h"

namespace companion {
// Session-owned readers; one record bank plus a bounded coverage bitmap.
class InventoryPairValidation {
 public:
  InventoryPairValidation(InventoryIndexStorage& index, InventoryIndexStorage& paths, std::span<uint8_t> scratch)
      : catalog(index, scratch.first(std::min(scratch.size(), INVENTORY_PATH_MAX_RECORD))),
        map(paths, scratch.first(std::min(scratch.size(), INVENTORY_PATH_MAX_RECORD))),
        coverage(scratch.subspan(std::min(scratch.size(), INVENTORY_PATH_MAX_RECORD))) {}
  bool validate(const Identity& generation, uint64_t revision = 0) {
    valid = false;
    if (coverage.empty() || coverage.size() > UINT64_MAX / 8 || !openIndex(generation) ||
        (revision != 0 && catalog.revision() != revision) || !openPaths(generation))
      return false;
    const uint64_t capacity = coverage.size() * 8;
    uint64_t base = 0;
    do {
      const uint64_t count = std::min<uint64_t>(capacity, catalog.count() - base);
      std::fill(coverage.begin(), coverage.end(), 0);
      map.rewind();
      for (;;) {
        ContentManifest manifest;
        const auto result = nextManifest(manifest);
        if (result == InventoryPathRecordResult::Error) return false;
        if (result == InventoryPathRecordResult::End) break;
        uint64_t index = 0;
        if (!locate(manifest, index)) return false;
        if (index >= base && index - base < count) {
          const uint64_t bit = index - base;
          coverage[bit / 8] |= static_cast<uint8_t>(1U << (bit % 8));
        }
      }
      for (uint64_t bit = 0; bit < count; ++bit) {
        if ((coverage[bit / 8] & (1U << (bit % 8))) == 0) return false;
      }
      base += count;
    } while (base < catalog.count());
    valid = true;
    return true;
  }
  uint64_t revision() const { return valid ? catalog.revision() : 0; }

 private:
  IndexedInventoryCatalog catalog;
  InventoryPaths map;
  std::span<uint8_t> coverage;
  bool valid = false;
  [[gnu::noinline]] bool openIndex(const Identity& generation) { return catalog.open(generation); }
  [[gnu::noinline]] bool openPaths(const Identity& generation) { return map.open(generation, catalog.revision()); }
  [[gnu::noinline]] InventoryPathRecordResult nextManifest(ContentManifest& manifest) { return map.next(manifest); }
  [[gnu::noinline]] bool locate(const ContentManifest& target, uint64_t& index) {
    uint64_t low = 0, high = catalog.count();
    while (low < high) {
      const uint64_t middle = low + (high - low) / 2;
      ContentManifest candidate;
      if (!catalog.read(middle, candidate)) return false;
      if (candidate.contentHash < target.contentHash)
        low = middle + 1;
      else
        high = middle;
    }
    if (low == catalog.count()) return false;
    ContentManifest candidate;
    if (!catalog.read(low, candidate) || candidate != target) return false;
    index = low;
    return true;
  }
};
}  // namespace companion
