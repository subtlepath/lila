#pragma once

#include "CompanionInventory.h"

namespace companion {
enum class InventoryResult : uint8_t { Ok, Invalid, Unauthorized, WrongStorage, Changed, IoError };
class InventoryCatalog {
 public:
  virtual ~InventoryCatalog() = default;
  virtual const Identity& storageGeneration() const = 0;
  virtual uint64_t revision() const = 0;
  virtual uint64_t count() const = 0;
  virtual bool read(uint64_t index, ContentManifest& manifest) = 0;
};
// Success is a result byte followed by a page; errors are a result byte only.
inline size_t handleInventory(InventoryCatalog& catalog, bool authorized, std::span<const uint8_t> body,
                              std::span<uint8_t> response) {
  if (response.empty()) return 0;
  response[0] = static_cast<uint8_t>(InventoryResult::Unauthorized);
  if (!authorized) return 1;
  response[0] = static_cast<uint8_t>(InventoryResult::Invalid);
  InventoryRequest request;
  if (!decodeInventoryRequest(body, request)) return 1;
  response[0] = static_cast<uint8_t>(InventoryResult::WrongStorage);
  if (request.storageGeneration != catalog.storageGeneration()) return 1;
  const uint64_t revision = catalog.revision(), total = catalog.count();
  response[0] = static_cast<uint8_t>(InventoryResult::Changed);
  if (revision == 0 || (request.revision != 0 && request.revision != revision)) return 1;
  response[0] = static_cast<uint8_t>(InventoryResult::Invalid);
  if (request.cursor > total) return 1;
  const size_t count = static_cast<size_t>(std::min<uint64_t>(total - request.cursor, request.limit));
  const size_t size = 1 + INVENTORY_PAGE_HEADER_SIZE + count * CONTENT_MANIFEST_SIZE;
  if (response.size() < size) return 1;
  response[0] = static_cast<uint8_t>(InventoryResult::IoError);
  for (size_t i = 0; i < count; ++i) {
    ContentManifest manifest;
    if (!catalog.read(request.cursor + i, manifest)) return 1;
    const auto kind = static_cast<uint8_t>(manifest.kind);
    if (kind < 1 || kind > 5) return 1;
    for (size_t j = 0; j < i; ++j) {
      const auto hash = response.subspan(1 + INVENTORY_PAGE_HEADER_SIZE + j * CONTENT_MANIFEST_SIZE + 2, 32);
      if (std::equal(manifest.contentHash.begin(), manifest.contentHash.end(), hash.begin())) return 1;
    }
    if (encodeRecord(manifest, response.subspan(1 + INVENTORY_PAGE_HEADER_SIZE + i * CONTENT_MANIFEST_SIZE,
                                                CONTENT_MANIFEST_SIZE)) != CONTENT_MANIFEST_SIZE)
      return 1;
  }
  response[0] = static_cast<uint8_t>(InventoryResult::Changed);
  if (catalog.storageGeneration() != request.storageGeneration || catalog.revision() != revision ||
      catalog.count() != total)
    return 1;
  InventoryPageHeader header;
  header.storageGeneration = request.storageGeneration;
  header.revision = revision;
  header.cursor = request.cursor;
  header.complete = total - request.cursor == count;
  header.nextCursor = header.complete ? 0 : request.cursor + count;
  response[0] = static_cast<uint8_t>(InventoryResult::Invalid);
  if (!encodeInventoryPageHeader(header, count, response.subspan(1))) return 1;
  response[0] = static_cast<uint8_t>(InventoryResult::Ok);
  return size;
}
}  // namespace companion
