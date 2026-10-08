#pragma once

#include "CompanionZipEntryMetadataValidation.h"
#include "CompanionZipHeaderNameValidation.h"
#include "CompanionZipNameAudit.h"
#include "CompanionZipRangeValidation.h"

namespace companion {
// Full metadata/payload/header-path sweep. Local overlap checks require the
// disk-backed range audit. Normalized duplicates require the supplied name audit.
class ZipArchiveValidation final {
 public:
  using Progress = bool (*)(void*);
  static constexpr uint64_t MAX_TOTAL_BYTES = 1024ULL * 1024 * 1024;
  ZipArchiveValidation(InventoryIndexStorage& source, std::span<uint8_t> scratch, tinfl_decompressor* decoder = nullptr,
                       std::span<uint8_t> window = {}, Progress progress = nullptr, void* context = nullptr,
                       uint64_t totalLimit = MAX_TOTAL_BYTES)
      : source(source),
        scratch(scratch),
        progress(progress),
        context(context),
        totalLimit(totalLimit),
        layoutParser(source, scratch, 20000, progress, context),
        entryParser(source, scratch, progress, context),
        nameParser(source, scratch, progress, context),
        extractor(source, scratch, decoder, window, progress, context) {}
  bool validate(ZipDirectoryLayout& output, ZipRangeValidation* ranges = nullptr, ZipNameAudit* names = nullptr) {
    struct Guard {
      ZipRangeValidation* ranges;
      ZipNameAudit* names;
      ~Guard() {
        if (ranges) ranges->abort();
        if (names) names->abort();
      }
    } guard{ranges, names};
    if (!layoutParser.validate(layout)) return false;
    if (ranges && !ranges->begin(layout.centralOffset)) return false;
    if (names && !names->begin()) return false;
    uint64_t at = layout.centralOffset, expanded = 0;
    for (uint64_t i = 0; i < layout.entries; ++i) {
      if (!entryParser.validate(layout, at, entry) || expanded > totalLimit ||
          entry.payload.expandedBytes > totalLimit - expanded ||
          !nameParser.validate(entry.nameOffset, entry.nameBytes, entry.payload.flags & 0x800, entry.directory, path) ||
          (ranges && !ranges->add(entry.localOffset, entry.localEnd)) || (names && !names->add(entry)) ||
          !extractor.extract(entry.payload, sink))
        return false;
      expanded += entry.payload.expandedBytes;
      at = entry.nextCentralOffset;
    }
    const auto end = layout.centralOffset + layout.centralBytes;
    if (at != end) {
      if (at > end || end - at < 6 || (progress && !progress(context)) || !source.read(at, scratch.first(6)) ||
          inventory_detail::read(scratch, 0, 4) != 0x05054b50 || inventory_detail::read(scratch, 4, 2) != end - at - 6)
        return false;
    }
    if (ranges && !ranges->finish()) return false;
    if (names && !names->finish()) return false;
    uint64_t finalLength = 0;
    if (!source.size(finalLength) || finalLength != layout.archiveBytes || (progress && !progress(context)))
      return false;
    output = layout;
    return true;
  }

 private:
  class CheckSink final : public ZipEntrySink {
   public:
    bool begin(uint64_t bytes) override {
      expected = bytes;
      extent = 0;
      return true;
    }
    bool write(uint64_t at, std::span<const uint8_t> bytes) override {
      if (at != extent || bytes.size() > expected - extent) return false;
      extent += bytes.size();
      return true;
    }
    bool seal(uint64_t bytes) override { return bytes == expected && extent == expected; }
    void abort() override { expected = extent = 0; }

   private:
    uint64_t expected = 0, extent = 0;
  } sink;
  InventoryIndexStorage& source;
  std::span<uint8_t> scratch;
  Progress progress;
  void* context;
  uint64_t totalLimit;
  ZipDirectoryLayoutValidation layoutParser;
  ZipEntryMetadataValidation entryParser;
  ZipHeaderNameValidation nameParser;
  ZipPathDetails path;
  ZipEntryExtraction extractor;
  ZipDirectoryLayout layout;
  ZipEntryMetadata entry;
};
}  // namespace companion
