#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "lib/Companion/CompanionMultiPathRemovalPlan.h"
using namespace companion;
namespace {
std::vector<uint8_t> fixture(const char* name) {
  std::ifstream file(std::string(COMPANION_FIXTURE_DIR) + "/" + name + ".json");
  const std::string json{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  const auto key = json.find("\"binaryHex\": \"");
  if (key == std::string::npos) return {};
  const auto start = key + 14;
  const auto end = json.find('"', start);
  if (end == std::string::npos || (end - start) % 2) return {};
  std::vector<uint8_t> bytes;
  bytes.reserve((end - start) / 2);
  for (size_t at = start; at < end; at += 2)
    bytes.push_back(static_cast<uint8_t>(std::stoul(json.substr(at, 2), nullptr, 16)));
  return bytes;
}
struct Storage final : InventoryIndexStorage {
  std::vector<uint8_t> bytes;
  unsigned reads = 0, failReadAt = 0;
  bool failSize = false;
  bool size(uint64_t& output) override {
    output = bytes.size();
    return !failSize;
  }
  bool read(uint64_t offset, std::span<uint8_t> output) override {
    if (++reads == failReadAt || offset > bytes.size() || output.size() > bytes.size() - offset) return false;
    std::copy_n(bytes.begin() + offset, output.size(), output.begin());
    return true;
  }
};
class MultiPathRemovalPlanTest : public testing::Test {
 protected:
  Storage storage;
  std::array<uint8_t, MULTI_PATH_REMOVAL_RECORD_MAX> scratch{};
  MultiPathRemovalPlanReader reader{storage, scratch};
  ContentRemovalRequest request;
  void SetUp() override {
    storage.bytes = fixture("MultiPathRemovalPlan");
    ASSERT_FALSE(storage.bytes.empty());
    ASSERT_TRUE(decodeContentRemovalRequest(fixture("ContentRemovalRequest"), request));
    request.manifest.kind = ContentKind::Epub;
  }
};
}  // namespace
TEST_F(MultiPathRemovalPlanTest, GoldenHeaderAndCopiedPathsRoundTripUnaligned) {
  MultiPathRemovalPlanHeader header;
  ASSERT_TRUE(decodeMultiPathRemovalPlanHeader(std::span(storage.bytes).first(MULTI_PATH_REMOVAL_HEADER_SIZE), header));
  EXPECT_EQ(header.request, request);
  EXPECT_EQ(header.inventoryRevision, 7U);
  EXPECT_EQ(header.count, 2U);
  std::array<uint8_t, MULTI_PATH_REMOVAL_HEADER_SIZE + 2> output{};
  output.fill(99);
  ASSERT_EQ(encodeMultiPathRemovalPlanHeader(header, std::span(output).subspan(1, MULTI_PATH_REMOVAL_HEADER_SIZE)),
            MULTI_PATH_REMOVAL_HEADER_SIZE);
  EXPECT_EQ(output.front(), 99);
  EXPECT_EQ(output.back(), 99);
  EXPECT_TRUE(std::equal(output.begin() + 1, output.end() - 1, storage.bytes.begin()));
  ASSERT_TRUE(reader.open(request));
  std::array<char, INVENTORY_PATH_LIMIT + 1> path{};
  EXPECT_EQ(reader.next(path), InventoryPathRecordResult::Entry);
  EXPECT_STREQ(path.data(), "/Books/original.epub");
  const auto retained = path;
  EXPECT_EQ(reader.next(path), InventoryPathRecordResult::Entry);
  EXPECT_STREQ(path.data(), "/elsewhere/renamed.epub");
  const auto last = path;
  EXPECT_EQ(reader.next(path), InventoryPathRecordResult::End);
  EXPECT_EQ(path, last);
  EXPECT_STREQ(retained.data(), "/Books/original.epub");
  reader.rewind();
  EXPECT_EQ(reader.next(path), InventoryPathRecordResult::Entry);
  EXPECT_STREQ(path.data(), "/Books/original.epub");
}
TEST_F(MultiPathRemovalPlanTest, EveryTruncationCorruptionAndTrailingByteIsRejected) {
  const auto original = storage.bytes;
  for (size_t length = 0; length < original.size(); ++length) {
    storage.bytes.assign(original.begin(), original.begin() + length);
    EXPECT_FALSE(reader.open(request)) << length;
    EXPECT_EQ(reader.current(), nullptr);
  }
  storage.bytes = original;
  for (size_t at = 0; at < original.size(); ++at) {
    storage.bytes[at] ^= 1;
    EXPECT_FALSE(reader.open(request)) << at;
    storage.bytes[at] ^= 1;
  }
  storage.bytes.push_back(0);
  EXPECT_FALSE(reader.open(request));
}
TEST_F(MultiPathRemovalPlanTest, FullOwnershipAndContentContractMustMatch) {
  for (unsigned field = 0; field < 7; ++field) {
    auto foreign = request;
    if (field == 0) foreign.transaction[0] ^= 1;
    if (field == 1) foreign.owner[0] ^= 1;
    if (field == 2) foreign.generation[0] ^= 1;
    if (field == 3) foreign.manifest.contentHash[0] ^= 1;
    if (field == 4) ++foreign.manifest.length;
    if (field == 5) ++foreign.manifest.formatVersion;
    if (field == 6) foreign.manifest.kind = ContentKind::Dictionary;
    EXPECT_FALSE(reader.open(foreign));
  }
  EXPECT_TRUE(reader.open(request));
}
TEST_F(MultiPathRemovalPlanTest, ImpossibleCountsLengthsAndKindsPreserveDecoderOutput) {
  MultiPathRemovalPlanHeader good;
  ASSERT_TRUE(decodeMultiPathRemovalPlanHeader(std::span(storage.bytes).first(MULTI_PATH_REMOVAL_HEADER_SIZE), good));
  for (unsigned invalid = 0; invalid < 6; ++invalid) {
    auto bad = good;
    if (invalid == 0) bad.count = 0;
    if (invalid == 1) bad.count = UINT64_MAX;
    if (invalid == 2) bad.recordsLength = UINT64_MAX;
    if (invalid == 3) bad.recordsLength = 0;
    if (invalid == 4) bad.inventoryRevision = 0;
    if (invalid == 5) bad.request.manifest.kind = ContentKind::Dictionary;
    scratch.fill(99);
    EXPECT_EQ(encodeMultiPathRemovalPlanHeader(bad, scratch), 0U);
    EXPECT_EQ(scratch.front(), 99);
  }
  auto bytes = storage.bytes;
  inventory_detail::write(bytes, 131, 0, 8);
  inventory_detail::write(bytes, 151, inventoryIndexCrc(std::span(bytes).first(151)), 4);
  auto output = good;
  EXPECT_FALSE(decodeMultiPathRemovalPlanHeader(std::span(bytes).first(MULTI_PATH_REMOVAL_HEADER_SIZE), output));
  EXPECT_EQ(output, good);
}
TEST_F(MultiPathRemovalPlanTest, RecordGrammarRejectsPrivatePathsBadUtf8AndWrongContent) {
  for (std::string_view path :
       {"/.crosspoint/book.epub", "/Books/../book.epub", "/Books/book.txt", "/Books/\xc0\xaf.epub"}) {
    scratch.fill(99);
    EXPECT_EQ(encodeMultiPathRemovalRecord(request, path, scratch), 0U);
    EXPECT_EQ(scratch.front(), 99);
  }
  const std::string maximum = "/" + std::string(INVENTORY_PATH_LIMIT - 6, 'a') + ".epub";
  ASSERT_EQ(maximum.size(), INVENTORY_PATH_LIMIT);
  ASSERT_EQ(encodeMultiPathRemovalRecord(request, maximum, scratch), scratch.size());
  std::string_view output;
  ASSERT_TRUE(decodeMultiPathRemovalRecord(request, scratch, output));
  EXPECT_EQ(output, maximum);
  for (size_t size = 0; size < scratch.size(); ++size) {
    output = "retained";
    EXPECT_FALSE(decodeMultiPathRemovalRecord(request, std::span(scratch).first(size), output));
    EXPECT_EQ(output, "retained");
  }
}
TEST_F(MultiPathRemovalPlanTest, OpenAndEnumerationIoFailuresDoNotPublishPathsOrAuthority) {
  ASSERT_TRUE(reader.open(request));
  const auto reads = storage.reads;
  for (unsigned failure = 1; failure <= reads; ++failure) {
    storage.reads = 0;
    storage.failReadAt = failure;
    EXPECT_FALSE(reader.open(request));
    EXPECT_EQ(reader.current(), nullptr);
  }
  storage.failReadAt = 0;
  storage.failSize = true;
  EXPECT_FALSE(reader.open(request));
  storage.failSize = false;
  ASSERT_TRUE(reader.open(request));
  std::array<char, INVENTORY_PATH_LIMIT + 1> path{};
  path.fill('x');
  storage.failReadAt = storage.reads + 1;
  EXPECT_EQ(reader.next(path), InventoryPathRecordResult::Error);
  EXPECT_EQ(path.front(), 'x');
  EXPECT_EQ(reader.current(), nullptr);
}
TEST_F(MultiPathRemovalPlanTest, CapacityScratchOverlapAndChangedHeaderInvalidateEnumeration) {
  ASSERT_TRUE(reader.open(request));
  std::array<char, INVENTORY_PATH_LIMIT + 1> path{};
  path.fill('x');
  EXPECT_EQ(reader.next(std::span(path).first(2)), InventoryPathRecordResult::Error);
  EXPECT_EQ(path.front(), 'x');
  ASSERT_TRUE(reader.open(request));
  const auto originalScratch = scratch;
  EXPECT_EQ(reader.next({reinterpret_cast<char*>(scratch.data()), scratch.size()}), InventoryPathRecordResult::Error);
  EXPECT_EQ(scratch, originalScratch);
  ASSERT_TRUE(reader.open(request));
  EXPECT_EQ(reader.next(path), InventoryPathRecordResult::Entry);
  EXPECT_EQ(reader.next(path), InventoryPathRecordResult::Entry);
  const auto previous = path;
  MultiPathRemovalPlanHeader header = *reader.current();
  ++header.inventoryRevision;
  ASSERT_EQ(encodeMultiPathRemovalPlanHeader(header, storage.bytes), MULTI_PATH_REMOVAL_HEADER_SIZE);
  EXPECT_EQ(reader.next(path), InventoryPathRecordResult::Error);
  EXPECT_EQ(path, previous);
  EXPECT_EQ(reader.current(), nullptr);
}

TEST_F(MultiPathRemovalPlanTest, BorrowedExpectedRequestCannotBeOverwrittenBeforeOwnershipCheck) {
  ASSERT_TRUE(reader.open(request));
  const auto& expected = reader.current()->request;
  auto foreign = *reader.current();
  foreign.request.owner.fill(99);
  ASSERT_EQ(encodeMultiPathRemovalPlanHeader(foreign, storage.bytes), MULTI_PATH_REMOVAL_HEADER_SIZE);
  EXPECT_FALSE(reader.open(expected));
  EXPECT_EQ(expected, request);
  EXPECT_EQ(reader.current(), nullptr);
}
TEST_F(MultiPathRemovalPlanTest, RewrittenValidRecordFooterCannotHidePayloadChangeAtEnd) {
  ASSERT_TRUE(reader.open(request));
  const size_t at = MULTI_PATH_REMOVAL_HEADER_SIZE;
  const size_t size = std::string_view("/Books/original.epub").size() + 6;
  ASSERT_EQ(encodeMultiPathRemovalRecord(request, "/Books/Original.epub", std::span(storage.bytes).subspan(at)), size);
  std::array<char, INVENTORY_PATH_LIMIT + 1> path{};
  EXPECT_EQ(reader.next(path), InventoryPathRecordResult::Entry);
  EXPECT_EQ(reader.next(path), InventoryPathRecordResult::Entry);
  const auto last = path;
  EXPECT_EQ(reader.next(path), InventoryPathRecordResult::Error);
  EXPECT_EQ(path, last);
  EXPECT_EQ(reader.current(), nullptr);
}
