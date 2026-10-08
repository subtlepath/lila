#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <iterator>
#include <vector>

#include "lib/Companion/CompanionZipEntryExtraction.h"
using namespace companion;
namespace {
struct Source : InventoryIndexStorage {
  std::vector<uint8_t> bytes;
  unsigned reads = 0, failRead = 0, sizes = 0, failSize = 0;
  bool changedSize = false;
  bool size(uint64_t& output) override {
    if (++sizes == failSize) return false;
    output = bytes.size() + (changedSize && sizes > 1 ? 1 : 0);
    return true;
  }
  bool read(uint64_t at, std::span<uint8_t> output) override {
    if (++reads == failRead || at > bytes.size() || output.size() > bytes.size() - at) return false;
    std::copy_n(bytes.begin() + at, output.size(), output.begin());
    return true;
  }
};
struct Sink : ZipEntrySink {
  std::vector<uint8_t> partial, sealed;
  unsigned writes = 0, failWrite = 0, aborts = 0, begins = 0;
  bool failBegin = false, failSeal = false;
  Sink() {
    partial.reserve(120000);
    sealed.reserve(120000);
  }
  bool begin(uint64_t) override {
    ++begins;
    partial.clear();
    writes = 0;
    return !failBegin;
  }
  bool write(uint64_t at, std::span<const uint8_t> bytes) override {
    if (++writes == failWrite || at != partial.size()) return false;
    partial.insert(partial.end(), bytes.begin(), bytes.end());
    return true;
  }
  bool seal(uint64_t bytes) override {
    if (failSeal || bytes != partial.size()) return false;
    sealed = partial;
    partial.clear();
    return true;
  }
  void abort() override {
    ++aborts;
    partial.clear();
  }
};
class ZipEntryExtractionTest : public testing::Test {
 protected:
  Source source;
  Sink sink;
  tinfl_decompressor decoder{};
  std::array<uint8_t, 32768> window;
  std::array<uint8_t, 512> scratch;
  ZipEntrySpan entry;
  void compressed() {
    source.sizes = 0;
    std::ifstream file(ZIP_ENTRY_FIXTURE, std::ios::binary);
    source.bytes = {std::istreambuf_iterator<char>(file), {}};
    entry = {0, source.bytes.size(), 120000, 0x7146dd0b, 8, 0x808};
  }
  void stored() {
    source.sizes = 0;
    source.bytes = {'a', 'b', 'c'};
    entry = {0, 3, 3, 0x352441c2, 0, 0x808};
  }
};
TEST_F(ZipEntryExtractionTest, DeflateWrapsHistoryAtEveryInputBankWidthWithoutExtraAllocations) {
  compressed();
  for (size_t width = 1; width <= scratch.size(); ++width) {
    ZipEntryExtraction extractor(source, std::span(scratch).first(width), &decoder, window);
    ASSERT_TRUE(extractor.extract(entry, sink)) << width;
    ASSERT_EQ(sink.sealed.size(), 120000u);
    for (size_t at = 0; at < sink.sealed.size(); ++at) ASSERT_EQ(sink.sealed[at], "abcdef"[at % 6]) << at;
  }
  EXPECT_EQ(sink.aborts, 0u);
}
TEST_F(ZipEntryExtractionTest, StoredOffsetsAndEmptyEntriesNeedNoDecoder) {
  stored();
  source.bytes.insert(source.bytes.begin(), {9, 8});
  entry.offset = 2;
  source.bytes.push_back(7);
  ZipEntryExtraction extractor(source, scratch);
  ASSERT_TRUE(extractor.extract(entry, sink));
  EXPECT_EQ(sink.sealed, (std::vector<uint8_t>{'a', 'b', 'c'}));
  entry = {source.bytes.size(), 0, 0, 0, 0, 0};
  ASSERT_TRUE(extractor.extract(entry, sink));
  EXPECT_TRUE(sink.sealed.empty());
}
TEST_F(ZipEntryExtractionTest, CorruptTruncatedTrailingAndWrongLengthStreamsNeverSeal) {
  for (unsigned invalid = 0; invalid < 6; ++invalid) {
    compressed();
    if (invalid == 0) entry.crc ^= 1;
    if (invalid == 1) --entry.expandedBytes;
    if (invalid == 2) ++entry.expandedBytes;
    if (invalid == 3) {
      source.bytes.pop_back();
      --entry.compressedBytes;
    }
    if (invalid == 4) {
      source.bytes.push_back(0);
      ++entry.compressedBytes;
    }
    if (invalid == 5) source.bytes[0] = 7;
    sink.sealed = {99};
    ZipEntryExtraction extractor(source, scratch, &decoder, window);
    EXPECT_FALSE(extractor.extract(entry, sink)) << invalid;
    EXPECT_EQ(sink.sealed, (std::vector<uint8_t>{99}));
    EXPECT_TRUE(sink.partial.empty());
  }
}
TEST_F(ZipEntryExtractionTest, UnfilledHistoryAndChangedSourceSizeAreRejected) {
  window.fill('A');
  source.bytes = {3, 2, 0};
  entry = {0, 3, 3, 0x66a031a7, 8, 0};
  ZipEntryExtraction extractor(source, scratch, &decoder, window);
  EXPECT_FALSE(extractor.extract(entry, sink));
  stored();
  source.changedSize = true;
  EXPECT_FALSE(extractor.extract(entry, sink));
  EXPECT_TRUE(sink.sealed.empty());
}
TEST_F(ZipEntryExtractionTest, AllReadWriteBeginSealFailuresAbortAndAllowRetry) {
  compressed();
  ZipEntryExtraction extractor(source, std::span(scratch).first(16), &decoder, window);
  ASSERT_TRUE(extractor.extract(entry, sink));
  const auto reads = source.reads, writes = sink.writes;
  for (unsigned failure = 1; failure <= reads; ++failure) {
    source.reads = 0;
    source.failRead = failure;
    const auto aborts = sink.aborts;
    EXPECT_FALSE(extractor.extract(entry, sink));
    EXPECT_EQ(sink.aborts, aborts + 1);
    EXPECT_TRUE(sink.partial.empty());
  }
  source.failRead = 0;
  for (unsigned failure = 1; failure <= writes; ++failure) {
    sink.failWrite = failure;
    EXPECT_FALSE(extractor.extract(entry, sink));
    EXPECT_TRUE(sink.partial.empty());
  }
  sink.failWrite = 0;
  sink.failBegin = true;
  EXPECT_FALSE(extractor.extract(entry, sink));
  sink.failBegin = false;
  sink.failSeal = true;
  EXPECT_FALSE(extractor.extract(entry, sink));
  sink.failSeal = false;
  ASSERT_TRUE(extractor.extract(entry, sink));
}
TEST_F(ZipEntryExtractionTest, UnsupportedFlagsBoundsAndCancellationDoNotPublish) {
  stored();
  const auto good = entry;
  for (unsigned invalid = 0; invalid < 6; ++invalid) {
    entry = good;
    if (invalid == 0) entry.flags |= 1;
    if (invalid == 1) entry.method = 9;
    if (invalid == 2) entry.offset = 99;
    if (invalid == 3) entry.expandedBytes = ZipEntryExtraction::MAX_EXPANDED_BYTES + 1;
    if (invalid == 4) ++entry.compressedBytes;
    if (invalid == 5) entry.flags |= 0x10;
    ZipEntryExtraction extractor(source, scratch, &decoder, window);
    EXPECT_FALSE(extractor.extract(entry, sink));
    EXPECT_EQ(sink.begins, 0u);
  }
  entry = good;
  ZipEntryExtraction cancelled(source, scratch, &decoder, window, [](void*) { return false; });
  EXPECT_FALSE(cancelled.extract(entry, sink));
  EXPECT_TRUE(sink.sealed.empty());
  EXPECT_EQ(sink.begins, 0u);
  EXPECT_EQ(sink.aborts, 0u);
}
TEST_F(ZipEntryExtractionTest, SourceSizeFailuresAndEmptyDeflateDoNotLeakOwnedStages) {
  compressed();
  ZipEntryExtraction extractor(source, scratch, &decoder, window);
  source.failSize = 1;
  EXPECT_FALSE(extractor.extract(entry, sink));
  EXPECT_EQ(sink.begins, 0u);
  source.sizes = 0;
  source.failSize = 2;
  EXPECT_FALSE(extractor.extract(entry, sink));
  EXPECT_TRUE(sink.partial.empty());
  EXPECT_EQ(sink.aborts, 1u);
  source.failSize = 0;
  source.bytes = {3, 0};
  entry = {0, 2, 0, 0, 8, 0};
  ASSERT_TRUE(extractor.extract(entry, sink));
  EXPECT_TRUE(sink.sealed.empty());
}
TEST_F(ZipEntryExtractionTest, CancellationAtEveryProgressPointPreservesSealedOutputAndAllowsRetry) {
  compressed();
  struct Cancellation {
    unsigned calls = 0, failAt = 0;
  } cancellation;
  auto progress = [](void* context) {
    auto& value = *static_cast<Cancellation*>(context);
    return ++value.calls != value.failAt;
  };
  ZipEntryExtraction extractor(source, std::span(scratch).first(16), &decoder, window, progress, &cancellation);
  ASSERT_TRUE(extractor.extract(entry, sink));
  const auto callbacks = cancellation.calls;
  const auto sealed = sink.sealed;
  ASSERT_GT(callbacks, 3u);
  for (unsigned at = 1; at <= callbacks; ++at) {
    cancellation = {0, at};
    const auto starts = sink.begins, aborts = sink.aborts;
    EXPECT_FALSE(extractor.extract(entry, sink)) << at;
    EXPECT_EQ(sink.sealed, sealed);
    EXPECT_TRUE(sink.partial.empty());
    EXPECT_EQ(sink.begins, starts + (at != 1));
    EXPECT_EQ(sink.aborts, aborts + (at != 1));
  }
  cancellation = {};
  ASSERT_TRUE(extractor.extract(entry, sink));
  EXPECT_EQ(sink.sealed, sealed);
}
}  // namespace
