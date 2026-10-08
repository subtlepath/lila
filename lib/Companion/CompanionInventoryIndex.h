#pragma once

#include "CompanionInventoryHandler.h"

namespace companion {
inline constexpr size_t INVENTORY_INDEX_HEADER_SIZE = 48;
inline constexpr size_t INVENTORY_INDEX_ENTRY_SIZE = CONTENT_MANIFEST_SIZE + 4;
struct InventoryIndexHeader {
  Identity generation{};
  uint64_t revision = 0;
  uint64_t count = 0;
  uint32_t entriesCrc = 0;
};
inline uint32_t inventoryIndexCrcUpdate(uint32_t value, std::span<const uint8_t> bytes) {
  for (uint8_t byte : bytes) {
    value ^= byte;
    for (unsigned bit = 0; bit < 8; ++bit) value = (value >> 1) ^ (0xEDB88320U & (0U - (value & 1U)));
  }
  return value;
}
inline uint32_t inventoryIndexCrc(std::span<const uint8_t> bytes) {
  return ~inventoryIndexCrcUpdate(0xFFFFFFFFU, bytes);
}
inline bool validInventoryIndexHeader(const InventoryIndexHeader& header) {
  return inventory_detail::nonzero(header.generation) && header.revision != 0 &&
         header.count <=
             (std::numeric_limits<uint64_t>::max() - INVENTORY_INDEX_HEADER_SIZE) / INVENTORY_INDEX_ENTRY_SIZE;
}
inline size_t encodeInventoryIndexHeader(const InventoryIndexHeader& header, std::span<uint8_t> output) {
  if (output.size() < INVENTORY_INDEX_HEADER_SIZE || !validInventoryIndexHeader(header)) return 0;
  static constexpr std::array<uint8_t, 8> PREFIX = {'C', 'I', 'D', 'X', 1, 0, 0, 0};
  std::copy(PREFIX.begin(), PREFIX.end(), output.begin());
  std::copy(header.generation.begin(), header.generation.end(), output.begin() + 8);
  inventory_detail::write(output, 24, header.revision, 8);
  inventory_detail::write(output, 32, header.count, 8);
  inventory_detail::write(output, 40, header.entriesCrc, 4);
  inventory_detail::write(output, 44, inventoryIndexCrc(output.first(44)), 4);
  return INVENTORY_INDEX_HEADER_SIZE;
}
inline bool decodeInventoryIndexHeader(std::span<const uint8_t> input, InventoryIndexHeader& output) {
  static constexpr std::array<uint8_t, 8> PREFIX = {'C', 'I', 'D', 'X', 1, 0, 0, 0};
  if (input.size() != INVENTORY_INDEX_HEADER_SIZE || !std::equal(PREFIX.begin(), PREFIX.end(), input.begin()) ||
      inventoryIndexCrc(input.first(44)) != inventory_detail::read(input, 44, 4))
    return false;
  InventoryIndexHeader header;
  std::copy_n(input.begin() + 8, 16, header.generation.begin());
  header.revision = inventory_detail::read(input, 24, 8);
  header.count = inventory_detail::read(input, 32, 8);
  header.entriesCrc = inventory_detail::read(input, 40, 4);
  if (!validInventoryIndexHeader(header)) return false;
  output = header;
  return true;
}
inline size_t encodeInventoryIndexEntry(const ContentManifest& manifest, std::span<uint8_t> output) {
  const auto kind = static_cast<uint8_t>(manifest.kind);
  if (output.size() < INVENTORY_INDEX_ENTRY_SIZE || kind < 1 || kind > 5 ||
      encodeRecord(manifest, output.first(CONTENT_MANIFEST_SIZE)) != CONTENT_MANIFEST_SIZE)
    return 0;
  inventory_detail::write(output, CONTENT_MANIFEST_SIZE, inventoryIndexCrc(output.first(CONTENT_MANIFEST_SIZE)), 4);
  return INVENTORY_INDEX_ENTRY_SIZE;
}
inline bool decodeInventoryIndexEntry(std::span<const uint8_t> input, ContentManifest& output) {
  return input.size() == INVENTORY_INDEX_ENTRY_SIZE &&
         inventoryIndexCrc(input.first(CONTENT_MANIFEST_SIZE)) ==
             inventory_detail::read(input, CONTENT_MANIFEST_SIZE, 4) &&
         decodeRecord(input.first(CONTENT_MANIFEST_SIZE), output);
}
// The provider owns one immutable snapshot handle and reports all I/O failures.
class InventoryIndexStorage {
 public:
  virtual ~InventoryIndexStorage() = default;
  virtual bool size(uint64_t& bytes) = 0;
  virtual bool read(uint64_t offset, std::span<uint8_t> output) = 0;
};
// Entries are strictly ordered by hash. Whole-file and per-entry CRCs detect
// torn snapshots and later read corruption; CRCs do not authenticate SD data.
class IndexedInventoryCatalog final : public InventoryCatalog {
 public:
  IndexedInventoryCatalog(InventoryIndexStorage& storage, std::span<uint8_t> scratch)
      : storage(storage), scratch(scratch) {}
  bool open(const Identity& generation) {
    ready = false;
    uint64_t bytes = 0;
    if (scratch.size() < INVENTORY_INDEX_ENTRY_SIZE || !storage.size(bytes) ||
        !storage.read(0, scratch.first(INVENTORY_INDEX_HEADER_SIZE)) ||
        !decodeInventoryIndexHeader(scratch.first(INVENTORY_INDEX_HEADER_SIZE), header) ||
        header.generation != generation ||
        bytes != INVENTORY_INDEX_HEADER_SIZE + header.count * INVENTORY_INDEX_ENTRY_SIZE)
      return false;
    uint32_t crc = 0xFFFFFFFFU;
    Digest previous{};
    for (uint64_t index = 0; index < header.count; ++index) {
      ContentManifest manifest;
      const auto buffer = scratch.first(INVENTORY_INDEX_ENTRY_SIZE);
      if (!storage.read(INVENTORY_INDEX_HEADER_SIZE + index * INVENTORY_INDEX_ENTRY_SIZE, buffer) ||
          !decodeInventoryIndexEntry(buffer, manifest) ||
          (index != 0 && !std::lexicographical_compare(previous.begin(), previous.end(), manifest.contentHash.begin(),
                                                       manifest.contentHash.end())))
        return false;
      previous = manifest.contentHash;
      crc = inventoryIndexCrcUpdate(crc, buffer);
    }
    if (~crc != header.entriesCrc) return false;
    ready = true;
    return true;
  }
  void invalidate() { ready = false; }
  const Identity& storageGeneration() const override { return header.generation; }
  uint64_t revision() const override { return ready ? header.revision : 0; }
  uint64_t count() const override { return ready ? header.count : 0; }
  bool read(uint64_t index, ContentManifest& manifest) override {
    if (!ready || index >= header.count) return false;
    if (!storage.read(INVENTORY_INDEX_HEADER_SIZE + index * INVENTORY_INDEX_ENTRY_SIZE,
                      scratch.first(INVENTORY_INDEX_ENTRY_SIZE)) ||
        !decodeInventoryIndexEntry(scratch.first(INVENTORY_INDEX_ENTRY_SIZE), manifest)) {
      invalidate();
      return false;
    }
    return true;
  }

 private:
  InventoryIndexStorage& storage;
  std::span<uint8_t> scratch;
  InventoryIndexHeader header{};
  bool ready = false;
};
}  // namespace companion
