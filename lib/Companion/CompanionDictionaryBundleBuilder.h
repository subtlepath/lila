#pragma once

#include <array>
#include <cstring>

#include "CompanionInventoryIndex.h"

namespace companion {
class DictionaryBundleSource {
 public:
  virtual ~DictionaryBundleSource() = default;
  // Validated members: data, idx, ifo, optional syn. Read fills the whole span.
  virtual bool size(unsigned member, uint64_t& bytes) = 0;
  virtual bool read(unsigned member, uint64_t offset, std::span<uint8_t> bytes) = 0;
  virtual bool close() = 0;
};
class DictionaryBundleStage {
 public:
  virtual ~DictionaryBundleStage() = default;
  virtual bool begin() = 0;
  virtual bool write(uint64_t offset, std::span<const uint8_t> bytes) = 0;
  // Seal only the private candidate. Installation/publication is caller-owned.
  virtual bool seal(uint64_t bytes) = 0;
  virtual void abort() = 0;
};
// Session-owned. Encodes canonical ZIP32, borrowing at least 64 bytes of scratch.
class DictionaryBundleBuilder final {
 public:
  DictionaryBundleBuilder(DictionaryBundleStage& stage, std::span<uint8_t> scratch) : stage(stage), scratch(scratch) {}
  DictionaryBundleBuilder(const DictionaryBundleBuilder&) = delete;
  DictionaryBundleBuilder& operator=(const DictionaryBundleBuilder&) = delete;
  bool build(DictionaryBundleSource& source, bool compressedData, bool synonyms, uint64_t& outputLength) {
    struct Guard {
      DictionaryBundleSource& source;
      DictionaryBundleStage& stage;
      bool staged = false, sealed = false;
      ~Guard() {
        source.close();
        if (staged && !sealed) stage.abort();
      }
    } guard{source, stage};
    if (scratch.size() < 64) return false;
    dictzip = compressedData;
    count = synonyms ? 4 : 3;
    offset = 0;
    uint64_t planned = 22;
    for (unsigned index = 0; index < count; ++index) {
      uint64_t size = 0;
      if (!source.size(index, size) || size > UINT32_MAX) return false;
      members[index] = {static_cast<uint32_t>(size), 0, 0};
      planned += size + 30 + 16 + 46 + 2 * std::strlen(name(index));
      if (planned > UINT32_MAX) return false;
    }
    guard.staged = true;
    if (!stage.begin()) return false;
    for (unsigned index = 0; index < count; ++index) {
      if (!member(source, index)) return false;
    }
    if (!source.close()) return false;
    const auto directory = offset;
    for (unsigned index = 0; index < count; ++index)
      if (!central(index)) return false;
    const auto directorySize = offset - directory;
    clear(22);
    put(0, 0x06054b50, 4);
    put(8, count, 2);
    put(10, count, 2);
    put(12, directorySize, 4);
    put(16, directory, 4);
    if (!append(scratch.first(22)) || offset != planned || !stage.seal(offset)) return false;
    guard.sealed = true;
    outputLength = offset;
    return true;
  }

 private:
  struct Member {
    uint32_t size, crc, offset;
  };
  std::array<Member, 4> members{};
  DictionaryBundleStage& stage;
  std::span<uint8_t> scratch;
  uint64_t offset = 0;
  unsigned count = 0;
  bool dictzip = false;
  static constexpr char NAMES[4][19] = {"dictionary.dict", "dictionary.idx", "dictionary.ifo", "dictionary.syn"};
  static constexpr char COMPRESSED_NAME[] = "dictionary.dict.dz";
  static constexpr uint16_t FLAGS = 0x0808;
  const char* name(unsigned index) const { return index == 0 && dictzip ? COMPRESSED_NAME : NAMES[index]; }
  void clear(size_t size) { std::fill_n(scratch.begin(), size, uint8_t{0}); }
  void put(size_t at, uint64_t value, unsigned width) { inventory_detail::write(scratch, at, value, width); }
  bool append(std::span<const uint8_t> bytes) {
    if (offset > UINT32_MAX || bytes.size() > UINT32_MAX - offset || !stage.write(offset, bytes)) return false;
    offset += bytes.size();
    return true;
  }
  bool member(DictionaryBundleSource& source, unsigned index) {
    auto& entry = members[index];
    entry.offset = static_cast<uint32_t>(offset);
    const auto nameLength = std::strlen(name(index));
    clear(30 + nameLength);
    put(0, 0x04034b50, 4);
    put(4, 20, 2);
    put(6, FLAGS, 2);
    put(12, 0x21, 2);
    put(26, nameLength, 2);
    std::memcpy(scratch.data() + 30, name(index), nameLength);
    if (!append(scratch.first(30 + nameLength))) return false;
    uint64_t consumed = 0;
    uint32_t crc = 0xffffffff;
    while (consumed < entry.size) {
      const auto chunk = static_cast<size_t>(std::min<uint64_t>(scratch.size(), uint64_t(entry.size) - consumed));
      auto bytes = scratch.first(chunk);
      if (!source.read(index, consumed, bytes)) return false;
      crc = inventoryIndexCrcUpdate(crc, bytes);
      if (!append(bytes)) return false;
      consumed += chunk;
    }
    uint64_t actualSize = 0;
    if (!source.size(index, actualSize) || actualSize != entry.size) return false;
    entry.crc = ~crc;
    clear(16);
    put(0, 0x08074b50, 4);
    put(4, entry.crc, 4);
    put(8, entry.size, 4);
    put(12, entry.size, 4);
    return append(scratch.first(16));
  }
  bool central(unsigned index) {
    const auto& entry = members[index];
    const auto nameLength = std::strlen(name(index));
    clear(46 + nameLength);
    put(0, 0x02014b50, 4);
    put(4, 20, 2);
    put(6, 20, 2);
    put(8, FLAGS, 2);
    put(14, 0x21, 2);
    put(16, entry.crc, 4);
    put(20, entry.size, 4);
    put(24, entry.size, 4);
    put(28, nameLength, 2);
    put(42, entry.offset, 4);
    std::memcpy(scratch.data() + 46, name(index), nameLength);
    return append(scratch.first(46 + nameLength));
  }
};
}  // namespace companion
