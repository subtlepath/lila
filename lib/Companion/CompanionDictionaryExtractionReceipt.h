#pragma once

#include "CompanionInventoryIndex.h"

namespace companion {
inline constexpr size_t DICTIONARY_EXTRACTION_RECEIPT_SIZE = 244;
// Parent-owned, session-resident. Persist before stage creation, then after
// each seal. Member order is definitions, index, info, synonyms.
struct DictionaryExtractionReceipt {
  uint64_t revision = 0;
  Identity transaction{}, generation{};
  Digest archiveHash{};
  std::array<uint64_t, 4> lengths{};
  std::array<Digest, 4> hashes{};
  uint8_t sealed = 0;
  bool compressed = false, synonyms = false;
  bool operator==(const DictionaryExtractionReceipt&) const = default;
};
inline bool dictionaryReceiptHashPresent(const Digest& hash) {
  return std::any_of(hash.begin(), hash.end(), [](uint8_t byte) { return byte != 0; });
}
inline bool validDictionaryExtractionReceipt(const DictionaryExtractionReceipt& receipt) {
  if (!receipt.revision || !inventory_detail::nonzero(receipt.transaction) ||
      !inventory_detail::nonzero(receipt.generation) || !dictionaryReceiptHashPresent(receipt.archiveHash) ||
      (receipt.sealed != 0 && receipt.sealed != 4 && receipt.sealed != 6 && receipt.sealed != 7 &&
       !(receipt.synonyms && receipt.sealed == 15)) ||
      receipt.lengths[2] > 65536)
    return false;
  for (unsigned at = 0; at < 4; ++at) {
    if (receipt.lengths[at] > 256ULL * 1024 * 1024 || (at == 3 && !receipt.synonyms && receipt.lengths[at] != 0))
      return false;
    const bool nonzero = dictionaryReceiptHashPresent(receipt.hashes[at]);
    if (nonzero != static_cast<bool>(receipt.sealed & (1u << at))) return false;
  }
  return true;
}
inline size_t encodeDictionaryExtractionReceipt(const DictionaryExtractionReceipt& receipt, std::span<uint8_t> output) {
  if (output.size() < DICTIONARY_EXTRACTION_RECEIPT_SIZE || !validDictionaryExtractionReceipt(receipt)) return 0;
  auto bytes = output.first(DICTIONARY_EXTRACTION_RECEIPT_SIZE);
  static constexpr std::array<uint8_t, 6> PREFIX = {'D', 'E', 'X', 'R', 1, 0};
  std::copy(PREFIX.begin(), PREFIX.end(), bytes.begin());
  bytes[6] = receipt.sealed;
  bytes[7] = (receipt.compressed ? 1 : 0) | (receipt.synonyms ? 2 : 0);
  inventory_detail::write(bytes, 8, receipt.revision, 8);
  std::copy(receipt.transaction.begin(), receipt.transaction.end(), bytes.begin() + 16);
  std::copy(receipt.generation.begin(), receipt.generation.end(), bytes.begin() + 32);
  std::copy(receipt.archiveHash.begin(), receipt.archiveHash.end(), bytes.begin() + 48);
  for (unsigned at = 0; at < 4; ++at) {
    inventory_detail::write(bytes, 80 + at * 8, receipt.lengths[at], 8);
    std::copy(receipt.hashes[at].begin(), receipt.hashes[at].end(), bytes.begin() + 112 + at * 32);
  }
  inventory_detail::write(bytes, 240, inventoryIndexCrc(bytes.first(240)), 4);
  return DICTIONARY_EXTRACTION_RECEIPT_SIZE;
}
inline bool decodeDictionaryExtractionReceipt(std::span<const uint8_t> bytes, DictionaryExtractionReceipt& output) {
  static constexpr std::array<uint8_t, 6> PREFIX = {'D', 'E', 'X', 'R', 1, 0};
  if (bytes.size() != DICTIONARY_EXTRACTION_RECEIPT_SIZE || !std::equal(PREFIX.begin(), PREFIX.end(), bytes.begin()) ||
      (bytes[7] & ~3u) || inventoryIndexCrc(bytes.first(240)) != inventory_detail::read(bytes, 240, 4))
    return false;
  const auto nonzero = [](std::span<const uint8_t> field) {
    return std::any_of(field.begin(), field.end(), [](uint8_t byte) { return byte != 0; });
  };
  const uint8_t mask = bytes[6];
  const bool synonyms = bytes[7] & 2;
  if (!inventory_detail::read(bytes, 8, 8) || !nonzero(bytes.subspan(16, 16)) || !nonzero(bytes.subspan(32, 16)) ||
      !nonzero(bytes.subspan(48, 32)) ||
      (mask != 0 && mask != 4 && mask != 6 && mask != 7 && !(synonyms && mask == 15)))
    return false;
  for (unsigned at = 0; at < 4; ++at) {
    const auto length = inventory_detail::read(bytes, 80 + at * 8, 8);
    if (length > 256ULL * 1024 * 1024 || (at == 2 && length > 65536) || (at == 3 && !synonyms && length != 0) ||
        nonzero(bytes.subspan(112 + at * 32, 32)) != static_cast<bool>(mask & (1u << at)))
      return false;
  }
  output.revision = inventory_detail::read(bytes, 8, 8);
  std::copy_n(bytes.begin() + 16, 16, output.transaction.begin());
  std::copy_n(bytes.begin() + 32, 16, output.generation.begin());
  std::copy_n(bytes.begin() + 48, 32, output.archiveHash.begin());
  output.sealed = bytes[6];
  output.compressed = bytes[7] & 1;
  output.synonyms = bytes[7] & 2;
  for (unsigned at = 0; at < 4; ++at) {
    output.lengths[at] = inventory_detail::read(bytes, 80 + at * 8, 8);
    std::copy_n(bytes.begin() + 112 + at * 32, 32, output.hashes[at].begin());
  }
  return true;
}
}  // namespace companion
