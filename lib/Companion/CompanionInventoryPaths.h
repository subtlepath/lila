#pragma once

#include <cstring>
#include <string_view>

#include "CompanionInventoryIndex.h"

namespace companion {
inline constexpr size_t INVENTORY_PATH_LIMIT = 511;
inline constexpr size_t INVENTORY_PATH_PREFIX = 2 + CONTENT_MANIFEST_SIZE + 2;
inline constexpr size_t INVENTORY_PATH_MAX_RECORD = INVENTORY_PATH_PREFIX + INVENTORY_PATH_LIMIT + 4;
struct InventoryPathRecord {
  ContentManifest manifest{};
  std::string_view path;
};
inline bool validInventoryPath(std::string_view path) {
  if (path.size() < 2 || path.size() > INVENTORY_PATH_LIMIT || path.front() != '/' || path.back() == '/') return false;
  size_t start = 1;
  for (size_t i = 1; i <= path.size(); ++i) {
    if (i == path.size() || path[i] == '/') {
      const auto component = path.substr(start, i - start);
      if (component.empty() || component == "." || component == "..") return false;
      start = i + 1;
    } else {
      const auto c = static_cast<unsigned char>(path[i]);
      if (c < 32 || c == 127 || c == '\\' || c == ':') return false;
    }
  }
  return true;
}
inline size_t encodeInventoryPath(const ContentManifest& manifest, std::string_view path, std::span<uint8_t> output) {
  const size_t size = INVENTORY_PATH_PREFIX + path.size() + 4;
  if (!validInventoryPath(path) || output.size() < size) return 0;
  output[0] = 'P';
  output[1] = 1;
  if (encodeRecord(manifest, output.subspan(2, CONTENT_MANIFEST_SIZE)) != CONTENT_MANIFEST_SIZE) return 0;
  inventory_detail::write(output, 2 + CONTENT_MANIFEST_SIZE, path.size(), 2);
  memcpy(output.data() + INVENTORY_PATH_PREFIX, path.data(), path.size());
  inventory_detail::write(output, size - 4, inventoryIndexCrc(output.first(size - 4)), 4);
  return size;
}
inline bool decodeInventoryPath(std::span<const uint8_t> bytes, InventoryPathRecord& output) {
  if (bytes.size() < INVENTORY_PATH_PREFIX + 6 || bytes[0] != 'P' || bytes[1] != 1) return false;
  const size_t length = inventory_detail::read(bytes, 2 + CONTENT_MANIFEST_SIZE, 2);
  if (bytes.size() != INVENTORY_PATH_PREFIX + length + 4 || length > INVENTORY_PATH_LIMIT ||
      inventory_detail::read(bytes, bytes.size() - 4, 4) != inventoryIndexCrc(bytes.first(bytes.size() - 4)))
    return false;
  InventoryPathRecord record;
  if (!decodeRecord(bytes.subspan(2, CONTENT_MANIFEST_SIZE), record.manifest)) return false;
  record.path = std::string_view(reinterpret_cast<const char*>(bytes.data() + INVENTORY_PATH_PREFIX), length);
  if (!validInventoryPath(record.path)) return false;
  output = record;
  return true;
}
inline size_t encodeInventoryPathsHeader(const InventoryIndexHeader& header, std::span<uint8_t> output) {
  if (!encodeInventoryIndexHeader(header, output)) return 0;
  output[1] = 'P';
  output[2] = 'T';
  output[3] = 'H';
  inventory_detail::write(output, 44, inventoryIndexCrc(output.first(44)), 4);
  return INVENTORY_INDEX_HEADER_SIZE;
}
inline bool decodeInventoryPathsHeader(std::span<const uint8_t> bytes, InventoryIndexHeader& output) {
  if (bytes.size() != INVENTORY_INDEX_HEADER_SIZE || bytes[0] != 'C' || bytes[1] != 'P' || bytes[2] != 'T' ||
      bytes[3] != 'H' || inventoryIndexCrc(bytes.first(44)) != inventory_detail::read(bytes, 44, 4))
    return false;
  std::array<uint8_t, INVENTORY_INDEX_HEADER_SIZE> copy;
  std::copy(bytes.begin(), bytes.end(), copy.begin());
  copy[1] = 'I';
  copy[2] = 'D';
  copy[3] = 'X';
  inventory_detail::write(copy, 44, inventoryIndexCrc(std::span(copy).first(44)), 4);
  return decodeInventoryIndexHeader(copy, output);
}
enum class InventoryPathResult { Found, Missing, Error };
enum class InventoryPathRecordResult { Entry, End, Error };
// Borrowed scratch contains variable-size records; paths never escape as views.
class InventoryPaths {
 public:
  InventoryPaths(InventoryIndexStorage& storage, std::span<uint8_t> scratch) : storage(storage), scratch(scratch) {}
  bool open(const Identity& generation, uint64_t revision) {
    ready = false;
    if (scratch.size() < INVENTORY_PATH_MAX_RECORD || !storage.size(length) ||
        !storage.read(0, scratch.first(INVENTORY_INDEX_HEADER_SIZE)) ||
        !decodeInventoryPathsHeader(scratch.first(INVENTORY_INDEX_HEADER_SIZE), header) ||
        header.generation != generation || header.revision != revision)
      return false;
    uint64_t offset = INVENTORY_INDEX_HEADER_SIZE;
    uint32_t crc = 0xffffffff;
    uint32_t payloadCrc = 0xffffffff;
    for (uint64_t i = 0; i < header.count; ++i) {
      InventoryPathRecord record;
      size_t size = 0;
      if (!read(offset, record, size)) return false;
      crc = inventoryIndexCrcUpdate(crc, scratch.first(size));
      payloadCrc = inventoryIndexCrcUpdate(payloadCrc, scratch.first(size - 4));
      offset += size;
    }
    if (offset != length || ~crc != header.entriesCrc) return false;
    openedPayloadCrc = ~payloadCrc;
    ready = true;
    rewind();
    return true;
  }
  bool belongsTo(const Identity& generation, uint64_t revision) const {
    return ready && header.generation == generation && header.revision == revision;
  }
  void rewind() {
    cursor = 0;
    cursorOffset = INVENTORY_INDEX_HEADER_SIZE;
    cursorCrc = 0xffffffff;
    cursorPayloadCrc = 0xffffffff;
  }
  InventoryPathRecordResult next(ContentManifest& manifest) {
    if (!ready) return InventoryPathRecordResult::Error;
    if (cursor == header.count) return finishEnumeration();
    InventoryPathRecord record;
    size_t size = 0;
    if (!read(cursorOffset, record, size)) {
      ready = false;
      return InventoryPathRecordResult::Error;
    }
    cursorCrc = inventoryIndexCrcUpdate(cursorCrc, scratch.first(size));
    cursorPayloadCrc = inventoryIndexCrcUpdate(cursorPayloadCrc, scratch.first(size - 4));
    cursorOffset += size;
    ++cursor;
    manifest = record.manifest;
    return InventoryPathRecordResult::Entry;
  }
  // Output is copied and terminated; it must not overlap borrowed scratch.
  // Any error invalidates enumeration and leaves both outputs unchanged.
  InventoryPathRecordResult nextPath(ContentManifest& manifest, std::span<char> output) {
    if (!ready) return InventoryPathRecordResult::Error;
    if (!disjoint(output)) {
      ready = false;
      return InventoryPathRecordResult::Error;
    }
    if (cursor == header.count) return finishEnumeration();
    InventoryPathRecord record;
    size_t size = 0;
    if (!read(cursorOffset, record, size) || output.size() <= record.path.size()) {
      ready = false;
      return InventoryPathRecordResult::Error;
    }
    memcpy(output.data(), record.path.data(), record.path.size());
    output[record.path.size()] = 0;
    manifest = record.manifest;
    cursorCrc = inventoryIndexCrcUpdate(cursorCrc, scratch.first(size));
    cursorPayloadCrc = inventoryIndexCrcUpdate(cursorPayloadCrc, scratch.first(size - 4));
    cursorOffset += size;
    ++cursor;
    return InventoryPathRecordResult::Entry;
  }
  InventoryPathResult find(const ContentManifest& manifest, std::span<char> output) {
    if (!ready) return InventoryPathResult::Error;
    uint64_t offset = INVENTORY_INDEX_HEADER_SIZE;
    for (uint64_t i = 0; i < header.count; ++i) {
      InventoryPathRecord record;
      size_t size = 0;
      if (!read(offset, record, size)) {
        ready = false;
        return InventoryPathResult::Error;
      }
      offset += size;
      if (record.manifest.contentHash != manifest.contentHash) continue;
      if (record.manifest != manifest || output.size() <= record.path.size()) return InventoryPathResult::Error;
      memcpy(output.data(), record.path.data(), record.path.size());
      output[record.path.size()] = 0;
      return InventoryPathResult::Found;
    }
    return InventoryPathResult::Missing;
  }

 private:
  InventoryIndexStorage& storage;
  std::span<uint8_t> scratch;
  InventoryIndexHeader header{};
  uint64_t length = 0;
  uint64_t cursor = 0, cursorOffset = INVENTORY_INDEX_HEADER_SIZE;
  bool ready = false;
  uint32_t cursorCrc = 0xffffffff;
  // Payload-only CRC avoids the fixed residue of records plus their CRC footer.
  uint32_t openedPayloadCrc = 0, cursorPayloadCrc = 0xffffffff;
  bool disjoint(std::span<char> output) const {
    const auto a = reinterpret_cast<uintptr_t>(output.data());
    const auto b = reinterpret_cast<uintptr_t>(scratch.data());
    return a <= b ? output.size() <= b - a : scratch.size() <= a - b;
  }
  InventoryPathRecordResult finishEnumeration() {
    InventoryIndexHeader current;
    uint64_t actualLength = 0;
    if (cursorOffset != length || ~cursorCrc != header.entriesCrc || ~cursorPayloadCrc != openedPayloadCrc ||
        !storage.size(actualLength) || actualLength != length ||
        !storage.read(0, scratch.first(INVENTORY_INDEX_HEADER_SIZE)) ||
        !decodeInventoryPathsHeader(scratch.first(INVENTORY_INDEX_HEADER_SIZE), current) ||
        current.generation != header.generation || current.revision != header.revision ||
        current.count != header.count || current.entriesCrc != header.entriesCrc) {
      ready = false;
      return InventoryPathRecordResult::Error;
    }
    return InventoryPathRecordResult::End;
  }
  bool read(uint64_t offset, InventoryPathRecord& record, size_t& size) {
    if (offset > length || INVENTORY_PATH_PREFIX > length - offset ||
        !storage.read(offset, scratch.first(INVENTORY_PATH_PREFIX)))
      return false;
    const size_t pathLength = inventory_detail::read(scratch, 2 + CONTENT_MANIFEST_SIZE, 2);
    if (pathLength < 2 || pathLength > INVENTORY_PATH_LIMIT) return false;
    size = INVENTORY_PATH_PREFIX + pathLength + 4;
    if (size > length - offset || !storage.read(offset, scratch.first(size))) return false;
    return decodeInventoryPath(scratch.first(size), record);
  }
};
}  // namespace companion
