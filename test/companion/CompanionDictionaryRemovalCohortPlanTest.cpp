#include <gtest/gtest.h>

#include <vector>

#include "lib/Companion/CompanionDictionaryRemovalCohortPlan.h"
using namespace companion;
namespace {
DictionaryRemovalPlan plan(bool compressed = false, bool synonyms = false) {
  DictionaryRemovalPlan result;
  result.request.transaction.fill(1);
  result.request.owner.fill(2);
  result.request.generation.fill(3);
  result.request.manifest.kind = ContentKind::Dictionary;
  result.request.manifest.formatVersion = 1;
  result.request.manifest.length = 1000;
  result.request.manifest.contentHash.fill(4);
  auto& installed = result.installed;
  installed.revision = 1;
  installed.phase = DictionaryInstallationPhase::Committed;
  installed.extraction.revision = 1;
  installed.extraction.transaction = result.request.transaction;
  installed.extraction.generation = result.request.generation;
  installed.extraction.archiveHash = result.request.manifest.contentHash;
  installed.extraction.compressed = compressed;
  installed.extraction.synonyms = synonyms;
  installed.extraction.sealed = installed.published = synonyms ? 15 : 7;
  installed.extraction.lengths = {6, 24, 111, synonyms ? 12U : 0U};
  for (unsigned at = 0; at < (synonyms ? 4U : 3U); ++at) installed.extraction.hashes[at].fill(at + 5);
  installed.archives.original = result.request.manifest;
  installed.archives.members = result.request.manifest;
  installed.archives.members.contentHash.fill(9);
  std::strcpy(installed.base.data(), "/dictionaries/Family/dictionary");
  return result;
}

struct Files final : InventoryIndexStorage {
  std::vector<uint8_t> bytes;
  uint64_t failOffset = UINT64_MAX;
  bool size(uint64_t& output) override {
    output = bytes.size();
    return true;
  }
  bool read(uint64_t offset, std::span<uint8_t> output) override {
    if (offset == failOffset || offset > bytes.size() || output.size() > bytes.size() - offset) return false;
    std::copy_n(bytes.begin() + offset, output.size(), output.begin());
    return true;
  }
};
std::vector<uint8_t> encode(const DictionaryRemovalPlan& first, const DictionaryRemovalPlan& second) {
  std::vector<uint8_t> bytes(DICTIONARY_REMOVAL_COHORT_HEADER_SIZE + 2 * DICTIONARY_REMOVAL_PLAN_SIZE);
  auto payload = std::span(bytes).subspan(DICTIONARY_REMOVAL_COHORT_HEADER_SIZE);
  EXPECT_EQ(DictionaryRemovalPlanCodec::encode(first, payload.first(DICTIONARY_REMOVAL_PLAN_SIZE)),
            DICTIONARY_REMOVAL_PLAN_SIZE);
  EXPECT_EQ(DictionaryRemovalPlanCodec::encode(second, payload.subspan(DICTIONARY_REMOVAL_PLAN_SIZE)),
            DICTIONARY_REMOVAL_PLAN_SIZE);
  DictionaryRemovalCohortHeader header{first.request, 7, 2, inventoryIndexCrc(payload)};
  EXPECT_EQ(encodeDictionaryRemovalCohortHeader(header, bytes), DICTIONARY_REMOVAL_COHORT_HEADER_SIZE);
  return bytes;
}
DictionaryRemovalPlan hidden() {
  auto result = plan(true, true);
  std::strcpy(result.installed.base.data(), "/.dictionaries/Family/dictionary");
  return result;
}
}  // namespace
TEST(DictionaryRemovalCohortPlan, StreamsEveryProofAndRewindsWithoutChangingEndOutput) {
  const auto first = hidden(), second = plan(true, true);
  Files files;
  files.bytes = encode(first, second);
  std::array<uint8_t, DICTIONARY_REMOVAL_PLAN_SIZE> scratch{};
  DictionaryRemovalCohortReader reader(files, scratch);
  ASSERT_TRUE(reader.open(first.request));
  ASSERT_NE(reader.current(), nullptr);
  EXPECT_EQ(reader.current()->inventoryRevision, 7U);
  EXPECT_EQ(reader.current()->count, 2U);
  DictionaryRemovalPlan output;
  for (unsigned repeat = 0; repeat < 2; ++repeat) {
    EXPECT_EQ(reader.next(output), InventoryPathRecordResult::Entry);
    EXPECT_EQ(output, first);
    EXPECT_EQ(reader.next(output), InventoryPathRecordResult::Entry);
    EXPECT_EQ(output, second);
    EXPECT_EQ(reader.next(output), InventoryPathRecordResult::End);
    EXPECT_EQ(output, second);
    reader.rewind();
  }
}
TEST(DictionaryRemovalCohortPlan, EveryTruncationAndByteCorruptionRefusesOpening) {
  const auto first = hidden(), second = plan(true, true);
  const auto valid = encode(first, second);
  std::array<uint8_t, DICTIONARY_REMOVAL_PLAN_SIZE> scratch{};
  Files files;
  DictionaryRemovalCohortReader reader(files, scratch);
  for (size_t length = 0; length < valid.size(); ++length) {
    files.bytes.assign(valid.begin(), valid.begin() + length);
    EXPECT_FALSE(reader.open(first.request)) << length;
    EXPECT_EQ(reader.current(), nullptr);
  }
  for (size_t at = 0; at < valid.size(); ++at) {
    files.bytes = valid;
    files.bytes[at] ^= 1;
    EXPECT_FALSE(reader.open(first.request)) << at;
    EXPECT_EQ(reader.current(), nullptr);
  }
  files.bytes = valid;
  files.bytes.push_back(0);
  EXPECT_FALSE(reader.open(first.request));
}
TEST(DictionaryRemovalCohortPlan, ValidChecksumsCannotAuthorizeDuplicateUnsortedOrForeignRecords) {
  auto first = hidden(), second = plan(true, true);
  Files files;
  std::array<uint8_t, DICTIONARY_REMOVAL_PLAN_SIZE> scratch{};
  DictionaryRemovalCohortReader reader(files, scratch);
  files.bytes = encode(first, first);
  EXPECT_FALSE(reader.open(first.request));
  files.bytes = encode(second, first);
  EXPECT_FALSE(reader.open(first.request));
  second.request.owner.fill(8);
  files.bytes = encode(first, second);
  EXPECT_FALSE(reader.open(first.request));
  auto request = first.request;
  request.generation.fill(8);
  files.bytes = encode(first, plan(true, true));
  EXPECT_FALSE(reader.open(request));
}
TEST(DictionaryRemovalCohortPlan, ReadFailuresAndChangedHeaderRefuseFurtherOutput) {
  const auto first = hidden(), second = plan(true, true);
  Files files;
  files.bytes = encode(first, second);
  std::array<uint8_t, DICTIONARY_REMOVAL_PLAN_SIZE> scratch{};
  DictionaryRemovalCohortReader reader(files, scratch);
  ASSERT_TRUE(reader.open(first.request));
  DictionaryRemovalPlan output = second;
  files.failOffset = DICTIONARY_REMOVAL_COHORT_HEADER_SIZE;
  EXPECT_EQ(reader.next(output), InventoryPathRecordResult::Error);
  EXPECT_EQ(output, second);
  EXPECT_EQ(reader.current(), nullptr);
  files.failOffset = UINT64_MAX;
  ASSERT_TRUE(reader.open(first.request));
  ASSERT_EQ(reader.next(output), InventoryPathRecordResult::Entry);
  ASSERT_EQ(reader.next(output), InventoryPathRecordResult::Entry);
  DictionaryRemovalCohortHeader changed{
      first.request, 8, 2, inventoryIndexCrc(std::span(files.bytes).subspan(DICTIONARY_REMOVAL_COHORT_HEADER_SIZE))};
  ASSERT_EQ(encodeDictionaryRemovalCohortHeader(changed, files.bytes), DICTIONARY_REMOVAL_COHORT_HEADER_SIZE);
  EXPECT_EQ(reader.next(output), InventoryPathRecordResult::Error);
  EXPECT_EQ(output, second);
}
TEST(DictionaryRemovalCohortPlan, HeaderBoundsAndShortScratchPreserveCallerValues) {
  const auto input = hidden();
  DictionaryRemovalCohortHeader header{input.request, 7, 2, 99}, output = header;
  std::array<uint8_t, DICTIONARY_REMOVAL_COHORT_HEADER_SIZE> bytes{};
  ASSERT_EQ(encodeDictionaryRemovalCohortHeader(header, bytes), bytes.size());
  for (size_t size = 0; size < bytes.size(); ++size) {
    EXPECT_FALSE(decodeDictionaryRemovalCohortHeader(std::span(bytes).first(size), output));
    EXPECT_EQ(output, header);
  }
  header.count = UINT64_MAX;
  EXPECT_EQ(encodeDictionaryRemovalCohortHeader(header, bytes), 0U);
  header.count = 0;
  EXPECT_EQ(encodeDictionaryRemovalCohortHeader(header, bytes), 0U);
  Files files;
  files.bytes = encode(input, plan(true, true));
  std::array<uint8_t, DICTIONARY_REMOVAL_PLAN_SIZE - 1> scratch{};
  DictionaryRemovalCohortReader reader(files, scratch);
  EXPECT_FALSE(reader.open(input.request));
}
