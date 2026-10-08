#include <gtest/gtest.h>

#include "lib/Companion/CompanionDictionaryArchiveBinding.h"
using namespace companion;
namespace {
DictionaryArchiveBinding binding() {
  DictionaryArchiveBinding value;
  value.members.kind = value.original.kind = ContentKind::Dictionary;
  value.members.formatVersion = value.original.formatVersion = 1;
  value.members.length = 123;
  value.original.length = 456;
  value.members.contentHash.fill(1);
  value.original.contentHash.fill(2);
  return value;
}
}  // namespace
TEST(DictionaryArchiveBindingTest, ExactRecordRoundTripIncludesPathIdentityAndUnalignedBytes) {
  Digest key{};
  key.fill(3);
  std::array<uint8_t, DICTIONARY_BINDING_SIZE + 2> bytes{};
  auto unaligned = std::span(bytes).subspan(1, DICTIONARY_BINDING_SIZE);
  const auto expected = binding();
  ASSERT_EQ(encodeDictionaryBinding(key, expected, unaligned), DICTIONARY_BINDING_SIZE);
  DictionaryArchiveBinding decoded;
  ASSERT_TRUE(decodeDictionaryBinding(key, unaligned, decoded));
  EXPECT_EQ(decoded, expected);
  key[0] ^= 1;
  EXPECT_FALSE(decodeDictionaryBinding(key, unaligned, decoded));
  EXPECT_EQ(decoded, expected);
  EXPECT_FALSE(decodeDictionaryBinding(key, bytes, decoded));
}
TEST(DictionaryArchiveBindingTest, EveryByteCorruptionAndTruncationPreservesOutput) {
  Digest key{};
  key.fill(3);
  std::array<uint8_t, DICTIONARY_BINDING_SIZE> bytes;
  const auto expected = binding();
  ASSERT_EQ(encodeDictionaryBinding(key, expected, bytes), bytes.size());
  for (size_t at = 0; at < bytes.size(); ++at) {
    auto corrupt = bytes;
    corrupt[at] ^= 1;
    auto decoded = expected;
    EXPECT_FALSE(decodeDictionaryBinding(key, corrupt, decoded)) << at;
    EXPECT_EQ(decoded, expected);
  }
  for (size_t size = 0; size < bytes.size(); ++size) {
    auto decoded = expected;
    EXPECT_FALSE(decodeDictionaryBinding(key, std::span(bytes).first(size), decoded)) << size;
    EXPECT_EQ(decoded, expected);
  }
}
TEST(DictionaryArchiveBindingTest, RejectsSemanticallyInvalidManifestsEvenWithCorrectChecksum) {
  Digest key{};
  std::array<uint8_t, DICTIONARY_BINDING_SIZE> bytes;
  for (unsigned member = 0; member < 2; ++member) {
    for (unsigned invalid = 0; invalid < 6; ++invalid) {
      auto value = binding();
      auto& manifest = member ? value.original : value.members;
      if (invalid == 0) manifest.kind = ContentKind::Epub;
      if (invalid == 1) manifest.formatVersion = 2;
      if (invalid == 2) manifest.length = 21;
      if (invalid == 3) manifest.length = uint64_t{UINT32_MAX} + 1;
      if (invalid == 4) manifest.logicalIdentity[0] = 1;
      if (invalid == 5) manifest.contentHash.fill(0);
      EXPECT_EQ(encodeDictionaryBinding(key, value, bytes), 0u);
      ASSERT_EQ(encodeDictionaryBinding(key, binding(), bytes), bytes.size());
      ASSERT_EQ(
          encodeRecord(manifest, std::span(bytes).subspan(40 + member * CONTENT_MANIFEST_SIZE, CONTENT_MANIFEST_SIZE)),
          CONTENT_MANIFEST_SIZE);
      inventory_detail::write(bytes, bytes.size() - 4, inventoryIndexCrc(std::span(bytes).first(bytes.size() - 4)), 4);
      auto decoded = binding();
      EXPECT_FALSE(decodeDictionaryBinding(key, bytes, decoded));
      EXPECT_EQ(decoded, binding());
    }
  }
}
