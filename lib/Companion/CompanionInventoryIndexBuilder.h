#pragma once

#include "CompanionInventoryIndex.h"

namespace companion {
enum class InventorySourceResult { Entry, End, Error };
class SortedInventorySource {
 public:
  virtual ~SortedInventorySource() = default;
  // End certifies a complete scan; failures must report Error.
  virtual InventorySourceResult next(ContentManifest& manifest) = 0;
};
class InventoryIndexSink {
 public:
  virtual ~InventoryIndexSink() = default;
  virtual bool begin() = 0;
  virtual bool write(uint64_t offset, std::span<const uint8_t> bytes) = 0;
  // Publish only on success, preserving the previous snapshot on failure.
  virtual bool finish(uint64_t bytes) = 0;
  virtual void abort() = 0;
};
// Borrowed scratch avoids a second buffer during SD enumeration.
class InventoryIndexBuilder {
 public:
  InventoryIndexBuilder(InventoryIndexSink& sink, std::span<uint8_t> scratch) : sink(sink), scratch(scratch) {}
  bool build(SortedInventorySource& source, const Identity& generation, uint64_t revision) {
    header = {generation, revision, 0, 0};
    if (scratch.size() < INVENTORY_INDEX_ENTRY_SIZE || !validInventoryIndexHeader(header)) return false;
    struct Guard {
      InventoryIndexSink& sink;
      bool committed = false;
      ~Guard() {
        if (!committed) sink.abort();
      }
    } guard{sink};
    if (!sink.begin() || !writeHeader()) return false;
    uint32_t crc = 0xFFFFFFFFU;
    bool havePrevious = false;
    ContentManifest manifest;
    for (;;) {
      const auto result = source.next(manifest);
      if (result == InventorySourceResult::Error) return false;
      if (result == InventorySourceResult::End) break;
      if (result != InventorySourceResult::Entry) return false;
      if (havePrevious) {
        if (previous.contentHash == manifest.contentHash) {
          if (previous != manifest) return false;
          continue;
        }
        if (!std::lexicographical_compare(previous.contentHash.begin(), previous.contentHash.end(),
                                          manifest.contentHash.begin(), manifest.contentHash.end()))
          return false;
      }
      if (header.count ==
          (std::numeric_limits<uint64_t>::max() - INVENTORY_INDEX_HEADER_SIZE) / INVENTORY_INDEX_ENTRY_SIZE)
        return false;
      const auto entry = scratch.first(INVENTORY_INDEX_ENTRY_SIZE);
      if (!encodeInventoryIndexEntry(manifest, entry) ||
          !sink.write(INVENTORY_INDEX_HEADER_SIZE + header.count * INVENTORY_INDEX_ENTRY_SIZE, entry))
        return false;
      crc = inventoryIndexCrcUpdate(crc, entry);
      ++header.count;
      previous = manifest;
      havePrevious = true;
    }
    header.entriesCrc = ~crc;
    if (!writeHeader() || !sink.finish(INVENTORY_INDEX_HEADER_SIZE + header.count * INVENTORY_INDEX_ENTRY_SIZE))
      return false;
    guard.committed = true;
    return true;
  }

 private:
  bool writeHeader() {
    return encodeInventoryIndexHeader(header, scratch) && sink.write(0, scratch.first(INVENTORY_INDEX_HEADER_SIZE));
  }
  InventoryIndexSink& sink;
  std::span<uint8_t> scratch;
  InventoryIndexHeader header{};
  ContentManifest previous{};
};
}  // namespace companion
