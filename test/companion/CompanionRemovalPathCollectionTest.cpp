#include <gtest/gtest.h>

#include "lib/Companion/CompanionRemovalPathCollection.h"
using namespace companion;
namespace {
struct Storage final : InventoryIndexStorage {
  std::vector<uint8_t> bytes;
  bool fail = false;
  bool size(uint64_t& size) override {
    size = bytes.size();
    return !fail;
  }
  bool read(uint64_t offset, std::span<uint8_t> output) override {
    if (fail || offset > bytes.size() || output.size() > bytes.size() - offset) return false;
    std::copy_n(bytes.begin() + offset, output.size(), output.begin());
    return true;
  }
};
struct Sink final : RemovalPathSink {
  Storage& storage;
  std::vector<std::string> paths;
  bool began = false, sealed = false, discarded = false;
  bool failBegin = false, failAppend = false, failFinish = false, failDiscard = false;
  bool failReadAfterAppend = false, changeHeaderAfterAppend = false, changeUnrelatedAfterAppend = false;
  uint64_t count = 0;
  explicit Sink(Storage& storage) : storage(storage) { paths.reserve(4); }
  bool begin(const ContentRemovalRequest&, uint64_t) override {
    began = true;
    return !failBegin;
  }
  bool append(std::string_view path) override {
    paths.emplace_back(path);
    if (failReadAfterAppend) storage.fail = true;
    if (changeUnrelatedAfterAppend) {
      const size_t second =
          INVENTORY_INDEX_HEADER_SIZE + INVENTORY_PATH_PREFIX + std::string_view("/Books/original.epub").size() + 4;
      InventoryPathRecord record;
      const size_t length = INVENTORY_PATH_PREFIX + std::string_view("/Books/unrelated.epub").size() + 4;
      EXPECT_TRUE(decodeInventoryPath(std::span(storage.bytes).subspan(second, length), record));
      EXPECT_EQ(encodeInventoryPath(record.manifest, "/Books/Unrelated.epub", std::span(storage.bytes).subspan(second)),
                length);
    }
    if (changeHeaderAfterAppend) {
      InventoryIndexHeader header;
      EXPECT_TRUE(decodeInventoryPathsHeader(std::span(storage.bytes).first(INVENTORY_INDEX_HEADER_SIZE), header));
      ++header.revision;
      EXPECT_EQ(encodeInventoryPathsHeader(header, storage.bytes), INVENTORY_INDEX_HEADER_SIZE);
    }
    return !failAppend;
  }
  bool finish(uint64_t size) override {
    count = size;
    sealed = !failFinish;
    return sealed;
  }
  bool discard() override {
    discarded = true;
    if (!sealed) paths.clear();
    return !failDiscard;
  }
};
class RemovalPathCollectionTest : public testing::Test {
 protected:
  Storage storage;
  Sink sink{storage};
  std::array<uint8_t, INVENTORY_PATH_MAX_RECORD> scratch{};
  InventoryPaths paths{storage, scratch};
  RemovalPathCollection collection{paths, sink};
  ContentRemovalRequest request;
  void SetUp() override {
    request.transaction.fill(1);
    request.owner.fill(2);
    request.generation.fill(3);
    request.manifest.kind = ContentKind::Epub;
    request.manifest.formatVersion = 1;
    request.manifest.length = 42;
    request.manifest.contentHash.fill(4);
    rebuild();
  }
  void rebuild(bool conflict = false, bool unrelatedLast = false) {
    storage.bytes.resize(INVENTORY_INDEX_HEADER_SIZE + 3 * INVENTORY_PATH_MAX_RECORD);
    size_t at = INVENTORY_INDEX_HEADER_SIZE;
    at += encodeInventoryPath(request.manifest, "/Books/original.epub", std::span(storage.bytes).subspan(at));
    auto other = request.manifest;
    other.contentHash.fill(5);
    at += encodeInventoryPath(other, "/Books/unrelated.epub", std::span(storage.bytes).subspan(at));
    auto last = request.manifest;
    if (conflict) ++last.length;
    if (unrelatedLast) last = other;
    at += encodeInventoryPath(last, "/elsewhere/renamed.epub", std::span(storage.bytes).subspan(at));
    storage.bytes.resize(at);
    InventoryIndexHeader header{request.generation, 7, 3,
                                inventoryIndexCrc(std::span(storage.bytes).subspan(INVENTORY_INDEX_HEADER_SIZE))};
    ASSERT_EQ(encodeInventoryPathsHeader(header, storage.bytes), INVENTORY_INDEX_HEADER_SIZE);
  }
};
}  // namespace
TEST_F(RemovalPathCollectionTest, StreamsEveryMatchingCopyAndSealsOnlyAtVerifiedEnd) {
  ASSERT_EQ(collection.collect(request, 7), RemovalPathCollectionResult::Ok);
  EXPECT_EQ(sink.paths, (std::vector<std::string>{"/Books/original.epub", "/elsewhere/renamed.epub"}));
  EXPECT_EQ(sink.count, 2U);
  EXPECT_TRUE(sink.sealed);
  EXPECT_FALSE(sink.discarded);
}
TEST_F(RemovalPathCollectionTest, InvalidRequestsAndWrongSnapshotsNeverBeginStaging) {
  EXPECT_EQ(collection.collect(request, 0), RemovalPathCollectionResult::Invalid);
  EXPECT_EQ(collection.collect(request, 8), RemovalPathCollectionResult::IoError);
  auto wrong = request;
  wrong.generation.fill(99);
  EXPECT_EQ(collection.collect(wrong, 7), RemovalPathCollectionResult::IoError);
  wrong = request;
  wrong.owner.fill(0);
  EXPECT_EQ(collection.collect(wrong, 7), RemovalPathCollectionResult::Invalid);
  EXPECT_FALSE(sink.began);
}
TEST_F(RemovalPathCollectionTest, MissingContentAndConflictingManifestDiscardPartialSet) {
  auto missing = request;
  missing.manifest.contentHash.fill(99);
  EXPECT_EQ(collection.collect(missing, 7), RemovalPathCollectionResult::Missing);
  EXPECT_TRUE(sink.discarded);
  EXPECT_FALSE(sink.sealed);
  rebuild(true);
  EXPECT_EQ(collection.collect(request, 7), RemovalPathCollectionResult::Conflict);
  EXPECT_TRUE(sink.paths.empty());
  EXPECT_FALSE(sink.sealed);
}
TEST_F(RemovalPathCollectionTest, LaterReadFailureAndChangedHeaderNeverSealPartialSet) {
  sink.failReadAfterAppend = true;
  EXPECT_EQ(collection.collect(request, 7), RemovalPathCollectionResult::IoError);
  EXPECT_TRUE(sink.discarded);
  EXPECT_TRUE(sink.paths.empty());
  EXPECT_FALSE(sink.sealed);
  storage.fail = sink.failReadAfterAppend = false;
  sink.changeHeaderAfterAppend = true;
  EXPECT_EQ(collection.collect(request, 7), RemovalPathCollectionResult::IoError);
  EXPECT_FALSE(sink.sealed);
  EXPECT_TRUE(sink.paths.empty());
}
TEST_F(RemovalPathCollectionTest, StagingFailuresNeverSealAndCleanupFailureRemainsAnError) {
  sink.failBegin = true;
  EXPECT_EQ(collection.collect(request, 7), RemovalPathCollectionResult::IoError);
  EXPECT_TRUE(sink.discarded);
  sink.failBegin = false;
  sink.failAppend = true;
  EXPECT_EQ(collection.collect(request, 7), RemovalPathCollectionResult::IoError);
  EXPECT_TRUE(sink.paths.empty());
  sink.failAppend = false;
  sink.failFinish = true;
  EXPECT_EQ(collection.collect(request, 7), RemovalPathCollectionResult::IoError);
  EXPECT_FALSE(sink.sealed);
  sink.failFinish = false;
  sink.failDiscard = true;
  auto missing = request;
  missing.manifest.contentHash.fill(99);
  EXPECT_EQ(collection.collect(missing, 7), RemovalPathCollectionResult::IoError);
}
TEST_F(RemovalPathCollectionTest, PathCopiesAreTerminatedAndDoNotAliasDecodeScratch) {
  ASSERT_TRUE(paths.open(request.generation, 7));
  ContentManifest output;
  std::array<char, INVENTORY_PATH_LIMIT + 1> name{};
  ASSERT_EQ(paths.nextPath(output, name), InventoryPathRecordResult::Entry);
  EXPECT_STREQ(name.data(), "/Books/original.epub");
  const auto retained = name;
  EXPECT_EQ(paths.next(output), InventoryPathRecordResult::Entry);
  EXPECT_EQ(name, retained);
  ASSERT_TRUE(paths.open(request.generation, 7));
  name.fill('x');
  output.length = 99;
  EXPECT_EQ(paths.nextPath(output, std::span(name).first(2)), InventoryPathRecordResult::Error);
  EXPECT_EQ(output.length, 99U);
  EXPECT_EQ(name[0], 'x');
  ASSERT_TRUE(paths.open(request.generation, 7));
  const auto previous = scratch;
  EXPECT_EQ(paths.nextPath(output, {reinterpret_cast<char*>(scratch.data()), scratch.size()}),
            InventoryPathRecordResult::Error);
  EXPECT_EQ(scratch, previous);
  EXPECT_EQ(output.length, 99U);
}

TEST_F(RemovalPathCollectionTest, ValidRecordCrcCannotHideChangedWholeSnapshot) {
  sink.changeUnrelatedAfterAppend = true;
  EXPECT_EQ(collection.collect(request, 7), RemovalPathCollectionResult::IoError);
  EXPECT_TRUE(sink.discarded);
  EXPECT_FALSE(sink.sealed);
  EXPECT_TRUE(sink.paths.empty());
}
