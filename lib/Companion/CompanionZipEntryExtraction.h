#pragma once

#include <MinizConfig.h>

#include "CompanionInventoryIndex.h"

namespace companion {
struct ZipEntrySpan {
  uint64_t offset = 0, compressedBytes = 0, expandedBytes = 0;
  uint32_t crc = 0;
  uint16_t method = 0, flags = 0;
};
class ZipEntrySink {
 public:
  virtual ~ZipEntrySink() = default;
  virtual bool begin(uint64_t expectedBytes) = 0;
  virtual bool write(uint64_t offset, std::span<const uint8_t> bytes) = 0;
  // A sealed private stage belongs to the caller, not the extractor.
  virtual bool seal(uint64_t bytes) = 0;
  virtual void abort() = 0;  // Remove only partial bytes owned by this sink.
};
// Session-owned; all storage and decoder buffers are borrowed. Archive headers,
// names, entry ranges and descriptors must be validated before calling extract.
class ZipEntryExtraction final {
 public:
  using Progress = bool (*)(void*);
  static constexpr uint64_t MAX_EXPANDED_BYTES = 256ULL * 1024 * 1024;
  ZipEntryExtraction(InventoryIndexStorage& source, std::span<uint8_t> scratch, tinfl_decompressor* decoder = nullptr,
                     std::span<uint8_t> window = {}, Progress progress = nullptr, void* context = nullptr)
      : source(source), scratch(scratch), decoder(decoder), window(window), progress(progress), context(context) {}
  bool extract(const ZipEntrySpan& entry, ZipEntrySink& sink) {
    uint64_t sourceBytes = 0;
    if (scratch.empty() || !source.size(sourceBytes) || entry.offset > sourceBytes ||
        entry.compressedBytes > sourceBytes - entry.offset || entry.expandedBytes > MAX_EXPANDED_BYTES ||
        (entry.method != 0 && entry.method != 8) || (entry.flags & ~(entry.method == 8 ? 0x80e : 0x808)) ||
        (entry.method == 0 && entry.compressedBytes != entry.expandedBytes) ||
        (entry.method == 8 && (!decoder || window.size() != 32768 || entry.compressedBytes == 0)))
      return false;
    struct Guard {
      ZipEntrySink& sink;
      bool started = false, sealed = false;
      ~Guard() {
        if (started && !sealed) sink.abort();
      }
    } guard{sink};
    if (!tick()) return false;
    guard.started = true;
    if (!sink.begin(entry.expandedBytes)) return false;
    uint32_t crc = 0xffffffff;
    if (entry.method == 0 ? !stored(entry, sink, crc) : !deflated(entry, sink, crc)) return false;
    uint64_t finalBytes = 0;
    if (~crc != entry.crc || !source.size(finalBytes) || finalBytes != sourceBytes || !tick() ||
        !sink.seal(entry.expandedBytes))
      return false;
    guard.sealed = true;
    return true;
  }

 private:
  InventoryIndexStorage& source;
  std::span<uint8_t> scratch;
  tinfl_decompressor* decoder;
  std::span<uint8_t> window;
  Progress progress;
  void* context;
  bool tick() const { return !progress || progress(context); }
  bool stored(const ZipEntrySpan& entry, ZipEntrySink& sink, uint32_t& crc) {
    for (uint64_t at = 0; at < entry.compressedBytes;) {
      const auto count = static_cast<size_t>(std::min<uint64_t>(scratch.size(), entry.compressedBytes - at));
      auto bytes = scratch.first(count);
      if (!tick() || !source.read(entry.offset + at, bytes) || !sink.write(at, bytes)) return false;
      crc = inventoryIndexCrcUpdate(crc, bytes);
      at += count;
    }
    return true;
  }
  bool deflated(const ZipEntrySpan& entry, ZipEntrySink& sink, uint32_t& crc) {
    tinfl_init(decoder);
    uint64_t loaded = 0, expanded = 0;
    size_t available = 0, consumed = 0, windowAt = 0;
    for (;;) {
      if (!tick()) return false;
      if (consumed == available) {
        consumed = 0;
        available = static_cast<size_t>(std::min<uint64_t>(scratch.size(), entry.compressedBytes - loaded));
        if (available && !source.read(entry.offset + loaded, scratch.first(available))) return false;
        loaded += available;
      }
      size_t inBytes = available - consumed, outBytes = window.size() - windowAt;
      const auto flags =
          TINFL_FLAG_VALIDATE_RING_HISTORY | (loaded < entry.compressedBytes ? TINFL_FLAG_HAS_MORE_INPUT : 0);
      const auto status = tinfl_decompress(decoder, scratch.data() + consumed, &inBytes, window.data(),
                                           window.data() + windowAt, &outBytes, flags);
      consumed += inBytes;
      if (outBytes > entry.expandedBytes - expanded) return false;
      const auto bytes = window.subspan(windowAt, outBytes);
      if (outBytes && !sink.write(expanded, bytes)) return false;
      crc = inventoryIndexCrcUpdate(crc, bytes);
      expanded += outBytes;
      windowAt = (windowAt + outBytes) % window.size();
      if (status == TINFL_STATUS_DONE)
        return expanded == entry.expandedBytes && loaded == entry.compressedBytes && consumed == available;
      if (status < TINFL_STATUS_DONE || (!inBytes && !outBytes)) return false;
    }
  }
};
}  // namespace companion
