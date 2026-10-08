#pragma once

#include "CompanionInventoryIndex.h"
#include "CompanionZipLegacyName.h"
#include "CompanionZipPathValidation.h"

namespace companion {
// Validates the header's own name encoding, not Unicode extra-field overrides
// or normalized duplicate destinations. Scratch is borrowed and split into
// raw/decoded banks; no archive-sized filename allocation is needed.
class ZipHeaderNameValidation final {
 public:
  using Progress = bool (*)(void*);
  ZipHeaderNameValidation(InventoryIndexStorage& source, std::span<uint8_t> scratch, Progress progress = nullptr,
                          void* context = nullptr)
      : source(source), scratch(scratch), progress(progress), context(context) {}
  bool validate(uint64_t offset, uint16_t bytes, bool utf8, bool directory, ZipPathDetails& output) {
    uint64_t length = 0;
    if (scratch.size() < 4 || !bytes || bytes > ZipPathValidation::MAX_BYTES || !source.size(length) ||
        offset > length || bytes > length - offset)
      return false;
    grammar.reset();
    const size_t bank = utf8 ? scratch.size() : scratch.size() / 4;
    for (size_t at = 0; at < bytes;) {
      const auto count = std::min<size_t>(bank, bytes - at);
      auto raw = scratch.first(count);
      if (!tick() || !source.read(offset + at, raw)) return false;
      if (utf8) {
        if (!grammar.consume(raw)) return false;
      } else {
        size_t decoded = 0;
        auto target = scratch.subspan(bank);
        if (!zipCp437ToUtf8(raw, target, decoded) || !grammar.consume(target.first(decoded))) return false;
      }
      at += count;
    }
    ZipPathDetails parsed;
    uint64_t finalLength = 0;
    if (!grammar.finish(directory, parsed) || !source.size(finalLength) || finalLength != length || !tick())
      return false;
    output = parsed;
    return true;
  }

 private:
  InventoryIndexStorage& source;
  std::span<uint8_t> scratch;
  Progress progress;
  void* context;
  ZipPathValidation grammar;
  bool tick() const { return !progress || progress(context); }
};
}  // namespace companion
