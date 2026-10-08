#include <gtest/gtest.h>

#include <array>
#include <numeric>
#include <random>
#include <vector>

#include "lib/Companion/CompanionInventorySorter.h"
using namespace companion;
namespace {
class Source final : public UnsortedInventorySource {
 public:
  std::vector<ContentManifest> entries;
  size_t cursor = 0;
  size_t errorAt = SIZE_MAX;
  InventorySourceResult next(ContentManifest& manifest) override {
    if (cursor == errorAt) return InventorySourceResult::Error;
    if (cursor == entries.size()) return InventorySourceResult::End;
    manifest = entries[cursor++];
    return InventorySourceResult::Entry;
  }
};
class Runs final : public InventorySortStorage {
 public:
  std::array<std::vector<uint8_t>, 2> files;
  int operation = 0, failAt = 0;
  bool corruptRead = false;
  size_t largestWrite = 0;
  Runs() {
    for (auto& file : files) file.reserve(100000);
  }
  bool fail() { return ++operation == failAt; }
  bool reset() override {
    if (fail()) return false;
    for (auto& file : files) file.clear();
    return true;
  }
  bool read(unsigned run, uint64_t offset, std::span<uint8_t> bytes) override {
    if (fail() || run > 1 || offset > files[run].size() || bytes.size() > files[run].size() - offset) return false;
    std::copy_n(files[run].begin() + offset, bytes.size(), bytes.begin());
    if (corruptRead && !bytes.empty()) bytes[0] ^= 1;
    return true;
  }
  bool write(unsigned run, uint64_t offset, std::span<const uint8_t> bytes) override {
    if (fail()) return false;
    auto& file = files.at(run);
    if (offset + bytes.size() > file.size()) file.resize(offset + bytes.size());
    std::copy(bytes.begin(), bytes.end(), file.begin() + offset);
    largestWrite = std::max(largestWrite, bytes.size());
    return true;
  }
  bool finish(unsigned run, uint64_t size) override {
    if (fail()) return false;
    files.at(run).resize(size);
    return true;
  }
};
ContentManifest manifest(uint32_t value) {
  ContentManifest result;
  for (size_t byte = 0; byte < 4; ++byte) result.contentHash[byte] = static_cast<uint8_t>(value >> (8 * (3 - byte)));
  result.kind = ContentKind::Epub;
  result.length = value + 1;
  return result;
}
Source source(size_t count) {
  Source result;
  result.entries.reserve(count);
  for (size_t index = 0; index < count; ++index) result.entries.push_back(manifest(index));
  std::mt19937 random(123);
  std::shuffle(result.entries.begin(), result.entries.end(), random);
  return result;
}
}  // namespace
TEST(CompanionInventorySorter, EmptyAndMultiPassCatalogsWithOddWorkspaceSizes) {
  for (const size_t scratchSize : {size_t{201}, size_t{202}, size_t{511}, size_t{1024}, size_t{4096}, size_t{8192}}) {
    for (const size_t count : {size_t{0}, size_t{1}, size_t{3}, size_t{4}, size_t{15}, size_t{257}, size_t{1001}}) {
      SCOPED_TRACE(testing::Message() << scratchSize << " " << count);
      Runs storage;
      std::vector<uint8_t> scratch(scratchSize, 0xab);
      auto input = source(count);
      InventorySorter sorter(storage, scratch);
      ASSERT_TRUE(sorter.build(input));
      ContentManifest decoded;
      for (size_t index = 0; index < count; ++index) {
        ASSERT_EQ(sorter.next(decoded), InventorySourceResult::Entry);
        EXPECT_EQ(decoded, manifest(index));
      }
      EXPECT_EQ(sorter.next(decoded), InventorySourceResult::End);
      EXPECT_LE(storage.largestWrite, scratchSize);
    }
  }
}
TEST(CompanionInventorySorter, EveryFailedStorageOperationPreventsCompletePublication) {
  Runs successful;
  std::array<uint8_t, 201> scratch{};
  auto input = source(15);
  InventorySorter first(successful, scratch);
  ASSERT_TRUE(first.build(input));
  const int operations = successful.operation;
  for (int failed = 1; failed <= operations; ++failed) {
    Runs storage;
    storage.failAt = failed;
    auto scan = source(15);
    InventorySorter sorter(storage, scratch);
    EXPECT_FALSE(sorter.build(scan)) << failed;
    ContentManifest decoded;
    EXPECT_EQ(sorter.next(decoded), InventorySourceResult::Error) << failed;
  }
}
TEST(CompanionInventorySorter, ScanFailureAndReadCorruptionNeverBecomeEnd) {
  Runs storage;
  std::array<uint8_t, 201> scratch{};
  for (size_t failure = 0; failure < 15; ++failure) {
    auto scan = source(15);
    scan.errorAt = failure;
    InventorySorter sorter(storage, scratch);
    EXPECT_FALSE(sorter.build(scan));
    ContentManifest retained = manifest(99);
    EXPECT_EQ(sorter.next(retained), InventorySourceResult::Error);
    EXPECT_EQ(retained, manifest(99));
  }
  auto scan = source(15);
  storage.corruptRead = true;
  InventorySorter sorter(storage, scratch);
  EXPECT_FALSE(sorter.build(scan));
  scan = source(15);
  storage.corruptRead = false;
  ASSERT_TRUE(sorter.build(scan));
  storage.corruptRead = true;
  ContentManifest retained = manifest(99);
  EXPECT_EQ(sorter.next(retained), InventorySourceResult::Error);
  EXPECT_EQ(retained, manifest(99));
  EXPECT_EQ(sorter.next(retained), InventorySourceResult::Error);
}
TEST(CompanionInventorySorter, RejectsSmallScratchAndPreservesDuplicateRecordsForIndexValidation) {
  Runs storage;
  std::array<uint8_t, 200> small{};
  auto scan = source(1);
  InventorySorter tooSmall(storage, small);
  EXPECT_FALSE(tooSmall.build(scan));
  EXPECT_EQ(storage.operation, 0);
  std::array<uint8_t, 201> scratch{};
  scan = source(10);
  scan.entries.reserve(12);
  scan.entries.push_back(manifest(3));
  auto conflict = manifest(3);
  conflict.length += 1;
  scan.entries.push_back(conflict);
  InventorySorter sorter(storage, scratch);
  ASSERT_TRUE(sorter.build(scan));
  ContentManifest decoded;
  unsigned duplicates = 0;
  while (sorter.next(decoded) == InventorySourceResult::Entry) {
    if (decoded.contentHash == manifest(3).contentHash) ++duplicates;
  }
  EXPECT_EQ(duplicates, 3);
}

TEST(CompanionInventorySorter, PreservesAllManifestFieldsAcrossMergePasses) {
  Runs storage;
  std::array<uint8_t, 511> scratch{};
  auto scan = source(257);
  for (auto& entry : scan.entries) {
    entry.kind = static_cast<ContentKind>(1 + entry.length % 5);
    entry.formatVersion = entry.length;
    entry.logicalIdentity.fill(static_cast<uint8_t>(entry.length));
  }
  auto expected = scan.entries;
  std::sort(expected.begin(), expected.end(),
            [](const auto& left, const auto& right) { return left.contentHash < right.contentHash; });
  InventorySorter sorter(storage, scratch);
  ASSERT_TRUE(sorter.build(scan));
  ContentManifest decoded;
  for (const auto& entry : expected) {
    ASSERT_EQ(sorter.next(decoded), InventorySourceResult::Entry);
    EXPECT_EQ(decoded, entry);
  }
  EXPECT_EQ(sorter.next(decoded), InventorySourceResult::End);
}
TEST(CompanionInventorySorter, IndexBuilderDeduplicatesOrRejectsConflictsBeforePublication) {
  class Sink final : public InventoryIndexSink {
   public:
    std::vector<uint8_t> candidate, published;
    bool aborted = false;
    Sink() {
      candidate.reserve(100000);
      published.reserve(100000);
      published.push_back(0xa5);
    }
    bool begin() override {
      candidate.clear();
      return true;
    }
    bool write(uint64_t offset, std::span<const uint8_t> bytes) override {
      if (offset + bytes.size() > candidate.size()) candidate.resize(offset + bytes.size());
      std::copy(bytes.begin(), bytes.end(), candidate.begin() + offset);
      return true;
    }
    bool finish(uint64_t size) override {
      if (candidate.size() != size) return false;
      published = candidate;
      return true;
    }
    void abort() override { aborted = true; }
  };
  for (bool conflict : {false, true}) {
    Runs storage;
    Sink sink;
    std::array<uint8_t, 201> scratch{};
    auto scan = source(10);
    scan.entries.reserve(11);
    auto duplicate = manifest(3);
    if (conflict) ++duplicate.length;
    scan.entries.push_back(duplicate);
    InventorySorter sorter(storage, scratch);
    ASSERT_TRUE(sorter.build(scan));
    Identity generation{};
    generation[0] = 1;
    InventoryIndexBuilder builder(sink, scratch);
    EXPECT_EQ(builder.build(sorter, generation, 1), !conflict);
    if (conflict) {
      EXPECT_TRUE(sink.aborted);
      EXPECT_EQ(sink.published, std::vector<uint8_t>({0xa5}));
    } else {
      InventoryIndexHeader header;
      ASSERT_TRUE(decodeInventoryIndexHeader(std::span(sink.published).first(INVENTORY_INDEX_HEADER_SIZE), header));
      EXPECT_EQ(header.count, 10);
      EXPECT_FALSE(sink.aborted);
    }
  }
}
