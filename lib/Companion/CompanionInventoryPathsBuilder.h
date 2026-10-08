#pragma once

#include "CompanionInventoryPathSink.h"
#include "CompanionInventoryPaths.h"

namespace companion {
class InventoryPathsStage {
 public:
  virtual ~InventoryPathsStage() = default;
  virtual bool begin() = 0;
  virtual bool write(uint64_t offset, std::span<const uint8_t> bytes) = 0;
  // Truncate, sync and close the private candidate; never publish it here.
  virtual bool seal(uint64_t bytes) = 0;
  virtual void abort() = 0;
};
// Scratch is reused during scan; it must not alias an active hash or sort span.
class InventoryPathsBuilder final : public InventoryPathSink {
 public:
  InventoryPathsBuilder(InventoryPathsStage& stage, std::span<uint8_t> scratch) : stage(stage), scratch(scratch) {}
  InventoryPathsBuilder(const InventoryPathsBuilder&) = delete;
  InventoryPathsBuilder& operator=(const InventoryPathsBuilder&) = delete;
  ~InventoryPathsBuilder() override {
    if (writing) stage.abort();
  }
  bool begin(const Identity& generation, uint64_t revision) {
    if (writing) stage.abort();
    writing = false;
    header = {generation, revision, 0, 0};
    offset = INVENTORY_INDEX_HEADER_SIZE;
    crc = 0xffffffff;
    if (scratch.size() < INVENTORY_PATH_MAX_RECORD || !validInventoryIndexHeader(header)) return false;
    writing = true;
    if (!stage.begin() || !writeHeader()) return fail();
    return true;
  }
  bool record(const ContentManifest& manifest, const char* path) override {
    if (!writing) return false;
    if (!path) return fail();
    const size_t length = strnlen(path, INVENTORY_PATH_LIMIT + 1);
    if (length > INVENTORY_PATH_LIMIT) return fail();
    const size_t size = encodeInventoryPath(manifest, std::string_view(path, length), scratch);
    if (!size || offset > UINT64_MAX - size || header.count == UINT64_MAX || !stage.write(offset, scratch.first(size)))
      return fail();
    crc = inventoryIndexCrcUpdate(crc, scratch.first(size));
    offset += size;
    ++header.count;
    return true;
  }
  bool seal() {
    if (!writing) return false;
    header.entriesCrc = ~crc;
    if (!writeHeader() || !stage.seal(offset)) return fail();
    writing = false;
    return true;
  }
  void abort() {
    if (writing) stage.abort();
    writing = false;
  }

 private:
  InventoryPathsStage& stage;
  std::span<uint8_t> scratch;
  InventoryIndexHeader header{};
  uint64_t offset = INVENTORY_INDEX_HEADER_SIZE;
  uint32_t crc = 0xffffffff;
  bool writing = false;
  bool writeHeader() {
    return encodeInventoryPathsHeader(header, scratch) && stage.write(0, scratch.first(INVENTORY_INDEX_HEADER_SIZE));
  }
  bool fail() {
    abort();
    return false;
  }
};
}  // namespace companion
