#include <gtest/gtest.h>

#include <vector>

#include "../../lib/Companion/CompanionInventoryIndexBuilder.h"

using namespace companion;
namespace {
struct Source : SortedInventorySource {
  std::vector<ContentManifest> entries;
  size_t cursor = 0;
  bool fail = false;
  InventorySourceResult next(ContentManifest& value) override {
    if (cursor == entries.size()) return fail ? InventorySourceResult::Error : InventorySourceResult::End;
    value = entries[cursor++];
    return InventorySourceResult::Entry;
  }
};
struct Sink : InventoryIndexSink, InventoryIndexStorage {
  std::vector<uint8_t> candidate, published{42};
  bool failBegin = false, failWrite = false, failFinish = false;
  unsigned aborts = 0, begins = 0;
  bool begin() override {
    ++begins;
    candidate.clear();
    return !failBegin;
  }
  bool write(uint64_t offset, std::span<const uint8_t> bytes) override {
    if (failWrite) return false;
    if (offset + bytes.size() > candidate.size()) candidate.resize(offset + bytes.size());
    std::copy(bytes.begin(), bytes.end(), candidate.begin() + offset);
    return true;
  }
  bool finish(uint64_t bytes) override {
    if (failFinish || bytes != candidate.size()) return false;
    published = candidate;
    return true;
  }
  void abort() override {
    ++aborts;
    candidate.clear();
  }
  bool size(uint64_t& bytes) override {
    bytes = published.size();
    return true;
  }
  bool read(uint64_t offset, std::span<uint8_t> output) override {
    if (offset > published.size() || output.size() > published.size() - offset) return false;
    std::copy_n(published.begin() + offset, output.size(), output.begin());
    return true;
  }
};
Identity generation() {
  Identity result{};
  result[0] = 2;
  return result;
}
ContentManifest item(uint8_t hash) {
  ContentManifest value{};
  value.contentHash[0] = hash;
  value.kind = static_cast<ContentKind>(1);
  return value;
}
}  // namespace
TEST(CompanionInventoryBuilder, PublishesValidatedEmptyAndDeduplicatedSnapshots) {
  Sink sink;
  Source source;
  std::array<uint8_t, INVENTORY_INDEX_ENTRY_SIZE> scratch{};
  InventoryIndexBuilder builder(sink, scratch);
  ASSERT_TRUE(builder.build(source, generation(), 1));
  IndexedInventoryCatalog catalog(sink, scratch);
  ASSERT_TRUE(catalog.open(generation()));
  EXPECT_EQ(catalog.count(), 0);
  source.entries = {item(1), item(1), item(2)};
  ASSERT_TRUE(builder.build(source, generation(), 2));
  ASSERT_TRUE(catalog.open(generation()));
  EXPECT_EQ(catalog.count(), 2);
  ContentManifest result;
  ASSERT_TRUE(catalog.read(1, result));
  EXPECT_EQ(result, item(2));
  EXPECT_EQ(sink.aborts, 0);
}
TEST(CompanionInventoryBuilder, FailuresPreservePublishedSnapshot) {
  for (unsigned failure = 0; failure < 6; ++failure) {
    Sink sink;
    Source source;
    source.entries = {item(1), item(2)};
    source.fail = failure == 0;
    sink.failBegin = failure == 1;
    sink.failWrite = failure == 2;
    sink.failFinish = failure == 3;
    if (failure == 4) source.entries = {item(2), item(1)};
    if (failure == 5) {
      source.entries[1] = item(1);
      source.entries[1].length = 9;
    }
    std::array<uint8_t, INVENTORY_INDEX_ENTRY_SIZE> scratch{};
    InventoryIndexBuilder builder(sink, scratch);
    EXPECT_FALSE(builder.build(source, generation(), 1));
    EXPECT_EQ(sink.published, std::vector<uint8_t>{42});
    EXPECT_EQ(sink.aborts, 1);
    EXPECT_TRUE(sink.candidate.empty());
  }
}
TEST(CompanionInventoryBuilder, InvalidArgumentsDoNotBeginTransaction) {
  Sink sink;
  Source source;
  std::array<uint8_t, INVENTORY_INDEX_ENTRY_SIZE> scratch{};
  InventoryIndexBuilder builder(sink, scratch);
  EXPECT_FALSE(builder.build(source, Identity{}, 1));
  EXPECT_FALSE(builder.build(source, generation(), 0));
  InventoryIndexBuilder small(sink, std::span(scratch).first(48));
  EXPECT_FALSE(small.build(source, generation(), 1));
  EXPECT_EQ(sink.begins, 0);
  EXPECT_EQ(sink.aborts, 0);
}

TEST(CompanionInventoryBuilder, SupportsEveryManifestKindAndRejectsInvalidKinds) {
  Sink sink;
  Source source;
  source.entries.reserve(5);
  for (uint8_t kind = 1; kind <= 5; ++kind) {
    auto manifest = item(kind);
    manifest.kind = static_cast<ContentKind>(kind);
    source.entries.push_back(manifest);
  }
  std::array<uint8_t, INVENTORY_INDEX_ENTRY_SIZE> scratch{};
  InventoryIndexBuilder builder(sink, scratch);
  ASSERT_TRUE(builder.build(source, generation(), 1));
  IndexedInventoryCatalog catalog(sink, scratch);
  ASSERT_TRUE(catalog.open(generation()));
  ASSERT_EQ(catalog.count(), 5);
  for (uint64_t index = 0; index < 5; ++index) {
    ContentManifest manifest;
    ASSERT_TRUE(catalog.read(index, manifest));
    EXPECT_EQ(manifest, source.entries[index]);
  }
  const auto previous = sink.published;
  source.cursor = 0;
  source.entries[2].kind = static_cast<ContentKind>(6);
  EXPECT_FALSE(builder.build(source, generation(), 2));
  EXPECT_EQ(sink.published, previous);
  EXPECT_EQ(sink.aborts, 1);
}
