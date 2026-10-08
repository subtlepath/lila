#pragma once

#include <algorithm>
#include <limits>

#include "CompanionRecords.h"

namespace companion {
inline constexpr size_t INVENTORY_REQUEST_SIZE = 34;
inline constexpr size_t INVENTORY_PAGE_HEADER_SIZE = 43;
inline constexpr size_t MAX_INVENTORY_ENTRIES = 8;
inline constexpr size_t MAX_INVENTORY_PAGE_SIZE =
    INVENTORY_PAGE_HEADER_SIZE + MAX_INVENTORY_ENTRIES * CONTENT_MANIFEST_SIZE;
struct InventoryRequest {
  Identity storageGeneration{};
  uint64_t revision = 0;
  uint64_t cursor = 0;
  uint8_t limit = MAX_INVENTORY_ENTRIES;
  bool operator==(const InventoryRequest&) const = default;
};
struct InventoryPageHeader {
  Identity storageGeneration{};
  uint64_t revision = 0;
  uint64_t cursor = 0;
  uint64_t nextCursor = 0;
  bool complete = false;
};
struct InventoryPageView {
  InventoryPageHeader header{};
  std::span<const uint8_t> entries{};
  size_t count() const { return entries.size() / CONTENT_MANIFEST_SIZE; }
};
namespace inventory_detail {
inline uint64_t read(std::span<const uint8_t> input, size_t offset, size_t size) {
  uint64_t value = 0;
  for (size_t i = 0; i < size; ++i) value |= static_cast<uint64_t>(input[offset + i]) << (8 * i);
  return value;
}
inline void write(std::span<uint8_t> output, size_t offset, uint64_t value, size_t size) {
  for (size_t i = 0; i < size; ++i) output[offset + i] = static_cast<uint8_t>(value >> (8 * i));
}
inline bool nonzero(const Identity& identity) {
  return std::any_of(identity.begin(), identity.end(), [](uint8_t value) { return value != 0; });
}
inline bool valid(const InventoryRequest& request) {
  return nonzero(request.storageGeneration) && request.limit > 0 && request.limit <= MAX_INVENTORY_ENTRIES &&
         (request.revision != 0 || request.cursor == 0);
}
inline bool valid(const InventoryPageHeader& header, size_t count) {
  return nonzero(header.storageGeneration) && header.revision != 0 && count <= MAX_INVENTORY_ENTRIES &&
         header.cursor <= std::numeric_limits<uint64_t>::max() - count &&
         (header.complete ? header.nextCursor == 0 : count > 0 && header.nextCursor == header.cursor + count);
}
}  // namespace inventory_detail
inline bool decodeInventoryRequest(std::span<const uint8_t> input, InventoryRequest& output) {
  if (input.size() != INVENTORY_REQUEST_SIZE || input[0] != 1) return false;
  InventoryRequest request;
  std::copy_n(input.begin() + 1, 16, request.storageGeneration.begin());
  request.revision = inventory_detail::read(input, 17, 8);
  request.cursor = inventory_detail::read(input, 25, 8);
  request.limit = input[33];
  if (!inventory_detail::valid(request)) return false;
  output = request;
  return true;
}
inline size_t encodeInventoryRequest(const InventoryRequest& request, std::span<uint8_t> output) {
  if (output.size() < INVENTORY_REQUEST_SIZE || !inventory_detail::valid(request)) return 0;
  output[0] = 1;
  std::copy(request.storageGeneration.begin(), request.storageGeneration.end(), output.begin() + 1);
  inventory_detail::write(output, 17, request.revision, 8);
  inventory_detail::write(output, 25, request.cursor, 8);
  output[33] = request.limit;
  return INVENTORY_REQUEST_SIZE;
}
inline bool decodeInventoryPage(std::span<const uint8_t> input, InventoryPageView& output) {
  if (input.size() < INVENTORY_PAGE_HEADER_SIZE || input[0] != 1 || input[41] > 1) return false;
  const size_t count = input[42];
  if (count > MAX_INVENTORY_ENTRIES || input.size() != INVENTORY_PAGE_HEADER_SIZE + count * CONTENT_MANIFEST_SIZE)
    return false;
  InventoryPageHeader header;
  std::copy_n(input.begin() + 1, 16, header.storageGeneration.begin());
  header.revision = inventory_detail::read(input, 17, 8);
  header.cursor = inventory_detail::read(input, 25, 8);
  header.nextCursor = inventory_detail::read(input, 33, 8);
  header.complete = input[41] != 0;
  if (!inventory_detail::valid(header, count)) return false;
  const auto entries = input.subspan(INVENTORY_PAGE_HEADER_SIZE);
  for (size_t i = 0; i < count; ++i) {
    ContentManifest manifest;
    if (!decodeRecord(entries.subspan(i * CONTENT_MANIFEST_SIZE, CONTENT_MANIFEST_SIZE), manifest)) return false;
    for (size_t j = 0; j < i; ++j) {
      const auto previous = entries.subspan(j * CONTENT_MANIFEST_SIZE + 2, 32);
      if (std::equal(manifest.contentHash.begin(), manifest.contentHash.end(), previous.begin())) return false;
    }
  }
  output.header = header;
  output.entries = entries;
  return true;
}
inline size_t encodeInventoryPageHeader(const InventoryPageHeader& header, size_t count, std::span<uint8_t> output) {
  if (output.size() < INVENTORY_PAGE_HEADER_SIZE || !inventory_detail::valid(header, count)) return 0;
  output[0] = 1;
  std::copy(header.storageGeneration.begin(), header.storageGeneration.end(), output.begin() + 1);
  inventory_detail::write(output, 17, header.revision, 8);
  inventory_detail::write(output, 25, header.cursor, 8);
  inventory_detail::write(output, 33, header.nextCursor, 8);
  output[41] = header.complete;
  output[42] = static_cast<uint8_t>(count);
  return INVENTORY_PAGE_HEADER_SIZE;
}
inline size_t encodeInventoryPage(const InventoryPageHeader& header, std::span<const ContentManifest> entries,
                                  std::span<uint8_t> output) {
  const size_t size = INVENTORY_PAGE_HEADER_SIZE + entries.size() * CONTENT_MANIFEST_SIZE;
  if (!inventory_detail::valid(header, entries.size()) || output.size() < size) return 0;
  for (size_t i = 0; i < entries.size(); ++i) {
    const auto kind = static_cast<uint8_t>(entries[i].kind);
    if (kind < 1 || kind > 5) return 0;
    for (size_t j = 0; j < i; ++j)
      if (entries[i].contentHash == entries[j].contentHash) return 0;
  }
  if (!encodeInventoryPageHeader(header, entries.size(), output)) return 0;
  for (size_t i = 0; i < entries.size(); ++i)
    if (encodeRecord(entries[i], output.subspan(INVENTORY_PAGE_HEADER_SIZE + i * CONTENT_MANIFEST_SIZE,
                                                CONTENT_MANIFEST_SIZE)) != CONTENT_MANIFEST_SIZE)
      return 0;
  return size;
}
}  // namespace companion
