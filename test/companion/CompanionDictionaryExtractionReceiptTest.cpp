#include <gtest/gtest.h>

#include "lib/Companion/CompanionDictionaryExtractionReceipt.h"
using namespace companion;
namespace {
DictionaryExtractionReceipt initial() {
  DictionaryExtractionReceipt receipt;
  receipt.revision = 1;
  receipt.transaction[0] = 1;
  receipt.generation[0] = 2;
  receipt.archiveHash[0] = 3;
  receipt.lengths = {6, 24, 111, 10};
  receipt.synonyms = true;
  return receipt;
}
TEST(DictionaryExtractionReceiptTest, EveryExtractionBoundaryRoundTripsFromUnalignedBytes) {
  auto receipt = initial();
  std::array<uint8_t, DICTIONARY_EXTRACTION_RECEIPT_SIZE + 1> buffer{};
  for (unsigned mask : {0u, 4u, 6u, 7u, 15u}) {
    receipt.sealed = mask;
    for (unsigned at = 0; at < 4; ++at) receipt.hashes[at][0] = mask & (1u << at) ? at + 1 : 0;
    auto bytes = std::span(buffer).subspan(1);
    ASSERT_EQ(encodeDictionaryExtractionReceipt(receipt, bytes), bytes.size());
    DictionaryExtractionReceipt result;
    ASSERT_TRUE(decodeDictionaryExtractionReceipt(bytes, result));
    EXPECT_EQ(result, receipt);
    ++receipt.revision;
  }
}
TEST(DictionaryExtractionReceiptTest, EveryCorruptedByteAndTruncationPreservesOutput) {
  auto receipt = initial();
  std::array<uint8_t, DICTIONARY_EXTRACTION_RECEIPT_SIZE> bytes{};
  ASSERT_EQ(encodeDictionaryExtractionReceipt(receipt, bytes), bytes.size());
  auto output = receipt;
  output.revision = 99;
  const auto unchanged = output;
  for (size_t at = 0; at < bytes.size(); ++at) {
    bytes[at] ^= 1;
    EXPECT_FALSE(decodeDictionaryExtractionReceipt(bytes, output)) << at;
    EXPECT_EQ(output, unchanged);
    bytes[at] ^= 1;
    EXPECT_FALSE(decodeDictionaryExtractionReceipt(std::span(bytes).first(at), output)) << at;
    EXPECT_EQ(output, unchanged);
  }
}
TEST(DictionaryExtractionReceiptTest, InvalidOwnershipMasksHashesAndLimitsCannotEncode) {
  const auto good = initial();
  std::array<uint8_t, DICTIONARY_EXTRACTION_RECEIPT_SIZE> bytes;
  bytes.fill(0x55);
  const auto unchanged = bytes;
  for (unsigned fault = 0; fault < 9; ++fault) {
    auto bad = good;
    if (fault == 0) bad.revision = 0;
    if (fault == 1) bad.transaction = {};
    if (fault == 2) bad.generation = {};
    if (fault == 3) bad.archiveHash = {};
    if (fault == 4) bad.sealed = 1;
    if (fault == 5) bad.hashes[0][0] = 1;
    if (fault == 6) bad.lengths[2] = 65537;
    if (fault == 7) bad.lengths[0] = 256ULL * 1024 * 1024 + 1;
    if (fault == 8) bad.synonyms = false;
    EXPECT_EQ(encodeDictionaryExtractionReceipt(bad, bytes), 0u) << fault;
    EXPECT_EQ(bytes, unchanged);
  }
}
}  // namespace
