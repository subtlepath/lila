#include <gtest/gtest.h>
#include <openssl/evp.h>

#include "../../lib/Companion/CompanionInventoryPathsBuilder.h"
#include "../../lib/hal/HalInventoryFileSource.h"
using namespace companion;
namespace {
struct Resolver : InventoryFileResolver {
  std::string failPath;
  bool invalidKind = false, rejectHash = false;
  bool verifyHashed(const char*, const ContentManifest&) override { return !rejectHash; }
  InventoryFileDecision resolve(const char* path, HalFile& file, ContentManifest& metadata) override {
    if (path == failPath) return InventoryFileDecision::Error;
    if (std::string(path) == "/.crosspoint") return InventoryFileDecision::Skip;
    if (file.isDirectory()) return InventoryFileDecision::Include;
    if (!std::string(path).ends_with(".epub")) return InventoryFileDecision::Skip;
    metadata.kind = invalidKind ? static_cast<ContentKind>(255) : ContentKind::Epub;
    metadata.formatVersion = 7;
    metadata.length = 999;
    metadata.contentHash.fill(42);
    return InventoryFileDecision::Include;
  }
};
struct Paths : InventoryPathSink {
  std::vector<std::pair<ContentManifest, std::string>> entries;
  bool fail = false;
  Paths() { entries.reserve(4); }
  bool record(const ContentManifest& manifest, const char* path) override {
    if (fail) return false;
    entries.emplace_back(manifest, path);
    return true;
  }
};
}  // namespace
class InventoryFileSourceTest : public testing::Test {
 protected:
  void SetUp() override {
    directory_test::state = {};
    auto& state = directory_test::state;
    state.directories["/"] = {{".crosspoint", true}, {"books", true}, {"notes.txt", false}};
    state.directories["/books"] = {{"renamed.epub", false}, {"another.epub", false}};
    state.files["/books/renamed.epub"] = {1, 2, 3};
    state.files["/books/another.epub"] = {4, 5};
  }
};
TEST_F(InventoryFileSourceTest, EmitsActualHashesAndStagesPathsWithExplicitPruning) {
  HalInventoryDirectoryWalker walker;
  Resolver resolver;
  Paths paths;
  std::array<uint8_t, 2> scratch;
  HalInventoryFileSource source(walker, resolver, paths, scratch);
  ASSERT_TRUE(source.begin());
  ContentManifest manifest;
  for (const char* path : {"/books/renamed.epub", "/books/another.epub"}) {
    ASSERT_EQ(source.next(manifest), InventorySourceResult::Entry);
    Digest expected{};
    const auto& bytes = directory_test::state.files.at(path);
    ASSERT_EQ(EVP_Digest(bytes.data(), bytes.size(), expected.data(), nullptr, EVP_sha256(), nullptr), 1);
    EXPECT_EQ(manifest.contentHash, expected);
    EXPECT_EQ(manifest.length, bytes.size());
    EXPECT_EQ(manifest.formatVersion, 7u);
    EXPECT_EQ(paths.entries.back().first, manifest);
    EXPECT_EQ(paths.entries.back().second, path);
  }
  EXPECT_EQ(source.next(manifest), InventorySourceResult::End);
  EXPECT_EQ(source.next(manifest), InventorySourceResult::End);
  EXPECT_EQ(paths.entries.size(), 2u);
  EXPECT_GT(directory_test::state.yields, 0u);
}
TEST_F(InventoryFileSourceTest, EveryPipelineFailureStaysErrorAndPreservesOutput) {
  for (unsigned failure = 0; failure < 6; ++failure) {
    HalInventoryDirectoryWalker walker;
    Resolver resolver;
    Paths paths;
    std::array<uint8_t, 2> scratch;
    directory_test::state.failReadPath.clear();
    directory_test::state.errorDirectory.clear();
    if (failure == 0) resolver.failPath = "/books/another.epub";
    if (failure == 1) directory_test::state.failReadPath = "/books/another.epub";
    HalInventoryFileSource source(walker, resolver, paths, scratch);
    ASSERT_TRUE(source.begin());
    ContentManifest manifest;
    ASSERT_EQ(source.next(manifest), InventorySourceResult::Entry);
    const auto previous = manifest;
    if (failure == 2) paths.fail = true;
    if (failure == 3) directory_test::state.errorDirectory = "/books";
    if (failure == 4) resolver.invalidKind = true;
    if (failure == 5) resolver.rejectHash = true;
    EXPECT_EQ(source.next(manifest), InventorySourceResult::Error);
    EXPECT_EQ(manifest, previous);
    EXPECT_EQ(paths.entries.size(), 1u);
    EXPECT_EQ(source.next(manifest), InventorySourceResult::Error);
  }
}
TEST_F(InventoryFileSourceTest, InvalidStartAndRestart) {
  HalInventoryDirectoryWalker walker;
  Resolver resolver;
  Paths paths;
  std::array<uint8_t, 2> scratch;
  HalInventoryFileSource invalid(walker, resolver, paths, {});
  ContentManifest manifest;
  EXPECT_FALSE(invalid.begin());
  EXPECT_EQ(invalid.next(manifest), InventorySourceResult::Error);
  HalInventoryFileSource source(walker, resolver, paths, scratch);
  EXPECT_FALSE(source.begin("/missing"));
  EXPECT_EQ(source.next(manifest), InventorySourceResult::Error);
  ASSERT_TRUE(source.begin());
  ASSERT_EQ(source.next(manifest), InventorySourceResult::Entry);
  ASSERT_TRUE(source.begin());
  ASSERT_EQ(source.next(manifest), InventorySourceResult::Entry);
  EXPECT_EQ(paths.entries.size(), 2u);
}

namespace {
struct Runs : InventorySortStorage {
  std::array<std::vector<uint8_t>, 2> files;
  Runs() {
    for (auto& file : files) file.reserve(2000);
  }
  bool reset() override {
    for (auto& file : files) file.clear();
    return true;
  }
  bool read(unsigned run, uint64_t offset, std::span<uint8_t> bytes) override {
    auto& file = files.at(run);
    if (offset > file.size() || bytes.size() > file.size() - offset) return false;
    std::copy_n(file.begin() + offset, bytes.size(), bytes.begin());
    return true;
  }
  bool write(unsigned run, uint64_t offset, std::span<const uint8_t> bytes) override {
    auto& file = files.at(run);
    file.resize(offset + bytes.size());
    std::copy(bytes.begin(), bytes.end(), file.begin() + offset);
    return true;
  }
  bool finish(unsigned run, uint64_t size) override {
    files.at(run).resize(size);
    return true;
  }
};
struct Snapshot : InventoryIndexSink, InventoryIndexStorage {
  std::vector<uint8_t> staged, published;
  Snapshot() {
    staged.reserve(2000);
    published.reserve(2000);
  }
  bool begin() override {
    staged.clear();
    return true;
  }
  bool write(uint64_t offset, std::span<const uint8_t> bytes) override {
    if (offset + bytes.size() > staged.size()) staged.resize(offset + bytes.size());
    std::copy(bytes.begin(), bytes.end(), staged.begin() + offset);
    return true;
  }
  bool finish(uint64_t size) override {
    staged.resize(size);
    published = staged;
    return true;
  }
  void abort() override { staged.clear(); }
  bool size(uint64_t& bytes) override {
    bytes = published.size();
    return true;
  }
  bool read(uint64_t offset, std::span<uint8_t> bytes) override {
    if (offset > published.size() || bytes.size() > published.size() - offset) return false;
    std::copy_n(published.begin() + offset, bytes.size(), bytes.begin());
    return true;
  }
};
}  // namespace
TEST_F(InventoryFileSourceTest, CompleteScanSortsDeduplicatesAndPublishesWhileFailedRescanPreservesSnapshot) {
  auto& state = directory_test::state;
  state.directories["/books"].push_back({"same-book.epub", false});
  state.files["/books/same-book.epub"] = state.files["/books/renamed.epub"];
  HalInventoryDirectoryWalker walker;
  Resolver resolver;
  Paths paths;
  Runs runs;
  Snapshot snapshot;
  std::array<uint8_t, 8192> workspace;
  auto hashScratch = std::span(workspace).first(1024);
  auto sortScratch = std::span(workspace).subspan(1024);
  HalInventoryFileSource source(walker, resolver, paths, hashScratch);
  InventorySorter sorter(runs, sortScratch);
  InventoryIndexBuilder builder(snapshot, sortScratch);
  Identity generation{};
  generation[0] = 1;
  ASSERT_TRUE(source.begin());
  ASSERT_TRUE(sorter.build(source));
  ASSERT_TRUE(builder.build(sorter, generation, 1));
  IndexedInventoryCatalog catalog(snapshot, sortScratch);
  ASSERT_TRUE(catalog.open(generation));
  EXPECT_EQ(catalog.count(), 2u);
  EXPECT_EQ(paths.entries.size(), 3u);
  for (size_t i = 0; i < 2; ++i) {
    ContentManifest manifest;
    ASSERT_TRUE(catalog.read(i, manifest));
    EXPECT_TRUE(std::any_of(paths.entries.begin(), paths.entries.end(),
                            [&](const auto& entry) { return entry.first == manifest; }));
  }
  const auto previous = snapshot.published;
  state.failReadPath = "/books/another.epub";
  ASSERT_TRUE(source.begin());
  EXPECT_FALSE(sorter.build(source));
  EXPECT_FALSE(builder.build(sorter, generation, 2));
  EXPECT_EQ(snapshot.published, previous);
}

namespace {
struct MapStage : Snapshot, InventoryPathsStage {
  bool begin() override { return Snapshot::begin(); }
  bool write(uint64_t offset, std::span<const uint8_t> bytes) override { return Snapshot::write(offset, bytes); }
  bool seal(uint64_t bytes) override { return Snapshot::finish(bytes); }
  void abort() override { Snapshot::abort(); }
};
}  // namespace
TEST_F(InventoryFileSourceTest, OneWorkspaceBuildsIndexAndMatchingReadablePathCandidate) {
  HalInventoryDirectoryWalker walker;
  Resolver resolver;
  Runs runs;
  Snapshot snapshot;
  MapStage mapStage;
  std::array<uint8_t, 8192> workspace;
  const auto hashScratch = std::span(workspace).first(1024);
  const auto pathScratch = std::span(workspace).subspan(1024, INVENTORY_PATH_MAX_RECORD);
  const auto sortScratch = std::span(workspace).subspan(1024 + INVENTORY_PATH_MAX_RECORD);
  InventoryPathsBuilder pathBuilder(mapStage, pathScratch);
  Identity generation{};
  generation[0] = 1;
  ASSERT_TRUE(pathBuilder.begin(generation, 4));
  HalInventoryFileSource source(walker, resolver, pathBuilder, hashScratch);
  ASSERT_TRUE(source.begin());
  InventorySorter sorter(runs, sortScratch);
  ASSERT_TRUE(sorter.build(source));
  ASSERT_TRUE(pathBuilder.seal());
  InventoryIndexBuilder indexBuilder(snapshot, sortScratch);
  ASSERT_TRUE(indexBuilder.build(sorter, generation, 4));
  IndexedInventoryCatalog catalog(snapshot, sortScratch);
  ASSERT_TRUE(catalog.open(generation));
  InventoryPaths paths(mapStage, pathScratch);
  ASSERT_TRUE(paths.open(generation, catalog.revision()));
  std::array<char, 512> path;
  for (size_t i = 0; i < catalog.count(); ++i) {
    ContentManifest manifest;
    ASSERT_TRUE(catalog.read(i, manifest));
    ASSERT_EQ(paths.find(manifest, path), InventoryPathResult::Found);
    EXPECT_TRUE(directory_test::state.files.contains(path.data()));
  }
}

namespace {
struct BundleResolver : Resolver {
  std::string archive = "/.crosspoint/companion/dictionary-test.zip";
  ContentManifest expected;
  InventoryFileDecision bundleDecision = InventoryFileDecision::Include;
  unsigned calls = 0;
  BundleResolver() {
    expected.kind = ContentKind::Dictionary;
    expected.formatVersion = 1;
    expected.length = 22;
    const std::vector<uint8_t> bytes(22, 7);
    directory_test::state.files[archive] = bytes;
    EVP_Digest(bytes.data(), bytes.size(), expected.contentHash.data(), nullptr, EVP_sha256(), nullptr);
  }
  InventoryFileDecision resolveBundle(const char* path, ContentManifest& metadata, const char*& output) override {
    if (std::string(path) != "/books") return InventoryFileDecision::Skip;
    ++calls;
    metadata = expected;
    output = archive.c_str();
    return bundleDecision;
  }
};
}  // namespace
TEST_F(InventoryFileSourceTest, DictionaryBundlesAreVerifiedEmittedAndPrunedWithRetainedHandle) {
  HalInventoryDirectoryWalker walker;
  BundleResolver resolver;
  Paths paths;
  std::array<uint8_t, 8> scratch;
  HalInventoryFileSource source(walker, resolver, paths, scratch);
  for (unsigned scan = 0; scan < 4; ++scan) {
    ASSERT_TRUE(source.begin());
    ContentManifest manifest;
    ASSERT_EQ(source.next(manifest), InventorySourceResult::Entry);
    EXPECT_EQ(manifest, resolver.expected);
    EXPECT_EQ(paths.entries.back().second, resolver.archive);
    ASSERT_EQ(source.next(manifest), InventorySourceResult::End);
    ASSERT_TRUE(source.closeScan());
    EXPECT_EQ(directory_test::state.preparations, 2u);
  }
  EXPECT_EQ(resolver.calls, 4u);
  EXPECT_EQ(paths.entries.size(), 4u);
}
TEST_F(InventoryFileSourceTest, BundleFailuresPreserveOutputAndDoNotStageUnverifiedPaths) {
  for (unsigned failure = 0; failure < 12; ++failure) {
    SetUp();
    HalInventoryDirectoryWalker walker;
    BundleResolver resolver;
    Paths paths;
    std::array<uint8_t, 8> scratch;
    HalInventoryFileSource source(walker, resolver, paths, scratch);
    ASSERT_TRUE(source.begin());
    if (failure == 0) resolver.bundleDecision = InventoryFileDecision::Error;
    if (failure == 1) resolver.expected.contentHash[0] ^= 1;
    if (failure == 2) ++resolver.expected.length;
    if (failure == 3) resolver.expected.formatVersion = 2;
    if (failure == 4) resolver.expected.logicalIdentity[0] = 1;
    if (failure == 5) directory_test::state.files.erase(resolver.archive);
    if (failure == 6) directory_test::state.failReadPath = resolver.archive;
    if (failure == 7) paths.fail = true;
    if (failure == 8) resolver.rejectHash = true;
    if (failure == 9) {
      directory_test::state.files.erase(resolver.archive);
      directory_test::state.directories[resolver.archive] = {};
    }
    if (failure == 10) {
      directory_test::state.files[DICTIONARY_CACHE_CANDIDATE] = directory_test::state.files.at(resolver.archive);
      resolver.archive = DICTIONARY_CACHE_CANDIDATE;
    }
    if (failure == 11) {
      directory_test::state.files[DICTIONARY_MEMBER_CANDIDATE] = directory_test::state.files.at(resolver.archive);
      resolver.archive = DICTIONARY_MEMBER_CANDIDATE;
    }
    ContentManifest manifest;
    manifest.length = 99;
    const auto previous = manifest;
    EXPECT_EQ(source.next(manifest), InventorySourceResult::Error) << failure;
    EXPECT_EQ(manifest, previous);
    EXPECT_TRUE(paths.entries.empty());
    EXPECT_EQ(source.next(manifest), InventorySourceResult::Error);
    ASSERT_TRUE(source.closeScan());
  }
}
TEST_F(InventoryFileSourceTest, SkippedBundleKeepsOrdinaryDirectoryTraversal) {
  HalInventoryDirectoryWalker walker;
  BundleResolver resolver;
  resolver.bundleDecision = InventoryFileDecision::Skip;
  Paths paths;
  std::array<uint8_t, 8> scratch;
  HalInventoryFileSource source(walker, resolver, paths, scratch);
  ASSERT_TRUE(source.begin());
  ContentManifest manifest;
  ASSERT_EQ(source.next(manifest), InventorySourceResult::Entry);
  EXPECT_EQ(manifest.kind, ContentKind::Epub);
  ASSERT_EQ(source.next(manifest), InventorySourceResult::Entry);
  EXPECT_EQ(source.next(manifest), InventorySourceResult::End);
  EXPECT_EQ(paths.entries.size(), 2u);
}
