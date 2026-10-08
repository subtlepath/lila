#pragma once

#include <MinizConfig.h>

#include "CompanionDictzipLayoutValidation.h"

namespace companion {
// The caller owns decoder state, a 32768-byte history window and input scratch.
// Keep these in the transfer session, outside the task stack, and reuse them.
class DictzipValidation final {
 public:
  using Progress = bool (*)(void*);
  DictzipValidation(InventoryIndexStorage& source, tinfl_decompressor& decoder, std::span<uint8_t> window,
                    std::span<uint8_t> scratch, Progress progress = nullptr, void* context = nullptr)
      : source(source), decoder(decoder), window(window), scratch(scratch), progress(progress), context(context) {}

  bool validate(DictzipLayout& output) {
    if (window.size() != 32768 || scratch.size() < 12 || !source.size(length)) return false;
    DictzipLayout parsed;
    DictzipLayoutValidation layout(source, scratch, progress, context);
    if (!layout.validate(parsed) || !headerCrc(parsed.dataOffset)) return false;
    uint32_t crc = 0xffffffff;
    uint64_t at = parsed.dataOffset;
    for (unsigned chunk = 0; chunk < parsed.chunks; ++chunk) {
      if (!source.read(parsed.tableOffset + uint64_t(chunk) * 2, scratch.first(2))) return false;
      const auto compressed = uint32_t(scratch[0]) | uint32_t(scratch[1]) << 8;
      const auto expected =
          std::min<uint64_t>(parsed.chunkLength, uint64_t(parsed.expandedBytes) - uint64_t(chunk) * parsed.chunkLength);
      if (!decode(at, compressed, expected, true, crc)) return false;
      at += compressed;
    }
    // The gzip stream's final block must consume the complete remaining body
    // and produce no bytes; chunk checks used synthetic empty final blocks.
    if (at >= length - 8 || !decode(at, length - 8 - at, 0, false, crc) || ~crc != parsed.crc) return false;
    uint64_t finalLength;
    if (!source.size(finalLength) || finalLength != length) return false;
    output = parsed;
    return true;
  }

 private:
  InventoryIndexStorage& source;
  tinfl_decompressor& decoder;
  std::span<uint8_t> window, scratch;
  Progress progress;
  void* context;
  uint64_t length = 0;

  bool tick() const { return !progress || progress(context); }
  bool headerCrc(uint64_t dataOffset) {
    if (!source.read(0, scratch.first(4))) return false;
    if (!(scratch[3] & 2)) return true;
    if (dataOffset < 2) return false;
    uint32_t crc = 0xffffffff;
    for (uint64_t at = 0; at < dataOffset - 2;) {
      const auto count = static_cast<size_t>(std::min<uint64_t>(scratch.size(), dataOffset - 2 - at));
      if (!tick() || !source.read(at, scratch.first(count))) return false;
      crc = inventoryIndexCrcUpdate(crc, scratch.first(count));
      at += count;
    }
    return source.read(dataOffset - 2, scratch.first(2)) &&
           ((~crc & 65535) == (uint32_t(scratch[0]) | uint32_t(scratch[1]) << 8));
  }
  bool decode(uint64_t at, uint64_t compressed, uint64_t expected, bool flushed, uint32_t& crc) {
    static constexpr uint8_t FINAL_BLOCK[] = {3, 0};
    tinfl_init(&decoder);
    size_t available = 0, consumed = 0, windowAt = 0;
    uint64_t loaded = 0, expanded = 0;
    bool appended = false;
    for (;;) {
      if (!tick()) return false;
      if (consumed == available) {
        consumed = 0;
        if (loaded < compressed) {
          available = static_cast<size_t>(std::min<uint64_t>(scratch.size(), compressed - loaded));
          if (!source.read(at + loaded, scratch.first(available))) return false;
          loaded += available;
        } else if (flushed && !appended) {
          std::copy(std::begin(FINAL_BLOCK), std::end(FINAL_BLOCK), scratch.begin());
          available = sizeof(FINAL_BLOCK);
          appended = true;
        } else {
          available = 0;
        }
      }
      size_t inBytes = available - consumed, outBytes = window.size() - windowAt;
      const auto flags = TINFL_FLAG_VALIDATE_RING_HISTORY |
                         ((loaded < compressed || (flushed && !appended)) ? TINFL_FLAG_HAS_MORE_INPUT : 0);
      const auto status = tinfl_decompress(&decoder, scratch.data() + consumed, &inBytes, window.data(),
                                           window.data() + windowAt, &outBytes, flags);
      consumed += inBytes;
      if (outBytes > expected - expanded) return false;
      crc = inventoryIndexCrcUpdate(crc, window.subspan(windowAt, outBytes));
      expanded += outBytes;
      windowAt = (windowAt + outBytes) % window.size();
      if (status == TINFL_STATUS_DONE)
        return expanded == expected && loaded == compressed && consumed == available && (!flushed || appended);
      if (status < TINFL_STATUS_DONE || (!inBytes && !outBytes)) return false;
    }
  }
};
}  // namespace companion
