#include <Memory.h>
#include <gtest/gtest.h>

#include <fstream>
#include <iterator>

#include "../../lib/Companion/CompanionInventoryBuild.h"
#include "../../lib/hal/HalInventoryIndexStage.h"
#include "../../lib/hal/HalInventoryPathsStage.h"
#include "../../lib/hal/HalInventoryPublicationValidator.h"
#include "../../lib/hal/HalInventoryRevisionAllocator.h"
#include "../../lib/hal/HalInventorySortStorage.h"
#include "../../lib/hal/HalTransferStorage.h"
#include "lib/hal/HalInventoryBaseResolver.h"
#include "lib/hal/HalInventoryBuildSession.h"
#include "lib/hal/HalInventoryCourseResolver.h"
#include "lib/hal/HalInventoryFontResolver.h"
#include "lib/hal/HalInventoryResolverSession.h"
using namespace companion;
namespace {
Identity generation() {
  Identity value{};
  value[0] = 1;
  return value;
}
struct Scan : InventoryScan {
  InventoryPathsBuilder& paths;
  unsigned cursor = 0, errorAt = 99, begins = 0;
  bool failBegin = false, failClose = false, closed = true;
  explicit Scan(InventoryPathsBuilder& paths) : paths(paths) {}
  bool beginScan() override {
    ++begins;
    cursor = 0;
    closed = false;
    return !failBegin;
  }
  bool closeScan() override {
    closed = true;
    return !failClose;
  }
  InventorySourceResult next(ContentManifest& manifest) override {
    if (cursor == errorAt) return InventorySourceResult::Error;
    if (cursor == 3) return InventorySourceResult::End;
    manifest = {};
    manifest.contentHash[0] = cursor == 1 ? 1 : 2;
    manifest.length = 99;
    const char* path = cursor == 0 ? "/books/two.epub" : cursor == 1 ? "/books/one.epub" : "/books/copy.epub";
    ++cursor;
    return paths.record(manifest, path) ? InventorySourceResult::Entry : InventorySourceResult::Error;
  }
};
class InventoryBuildTest : public testing::Test {
 protected:
  std::array<uint8_t, 8192> scratch;
  HalTransferStorage storage;
  HalInventoryPublicationValidator validator{scratch};
  InventoryPublication publication{storage, validator, scratch};
  InventoryRollback rollback{storage, validator, scratch};
  InventoryRecovery recovery{storage, publication, rollback};
  HalInventoryRevisionAllocator revisions;
  HalInventoryPathsStage pathStage;
  InventoryPathsBuilder paths{pathStage, std::span(scratch).first(INVENTORY_PATH_MAX_RECORD)};
  HalInventoryIndexStage indexStage;
  InventoryIndexBuilder index{indexStage, std::span(scratch).subspan(INVENTORY_PATH_MAX_RECORD)};
  HalInventorySortStorage runs;
  InventorySorter sorter{runs, std::span(scratch).subspan(INVENTORY_PATH_MAX_RECORD)};
  InventoryBuild build{storage, validator, revisions, recovery, publication,
                       paths,   pathStage, sorter,    index,    indexStage};
  Scan scan{paths};
  void SetUp() override { inventory_hal_test::state = {}; }
};
}  // namespace
TEST_F(InventoryBuildTest, ReservesRevisionScansDeduplicatesAndPublishesMatchingPair) {
  uint64_t revision = 42;
  ASSERT_EQ(build.build(scan, generation(), revision), InventoryPublicationResult::Ok);
  EXPECT_EQ(revision, 1u);
  EXPECT_TRUE(scan.closed);
  HalInventoryIndexStorage reader;
  ASSERT_TRUE(reader.open(InventoryPublication::INDEX));
  IndexedInventoryCatalog catalog(reader, scratch);
  ASSERT_TRUE(catalog.open(generation()));
  EXPECT_EQ(catalog.count(), 2u);
  EXPECT_EQ(catalog.revision(), 1u);
  ASSERT_TRUE(reader.close());
  EXPECT_EQ(validator.pair(InventoryPublication::INDEX, InventoryPublication::PATHS, generation(), revision),
            InventoryValidation::Valid);
  EXPECT_EQ(build.build(scan, generation(), revision), InventoryPublicationResult::Ok);
  EXPECT_EQ(revision, 2u);
}
TEST_F(InventoryBuildTest, FailedScanAndCloseKeepActivePairAndDoNotReuseReservedRevision) {
  uint64_t revision = 0;
  ASSERT_EQ(build.build(scan, generation(), revision), InventoryPublicationResult::Ok);
  const auto oldIndex = inventory_hal_test::state.files.at(InventoryPublication::INDEX);
  const auto oldPaths = inventory_hal_test::state.files.at(InventoryPublication::PATHS);
  scan.errorAt = 1;
  revision = 42;
  EXPECT_EQ(build.build(scan, generation(), revision), InventoryPublicationResult::IoError);
  EXPECT_EQ(revision, 42u);
  EXPECT_TRUE(scan.closed);
  EXPECT_EQ(inventory_hal_test::state.files.at(InventoryPublication::INDEX), oldIndex);
  EXPECT_EQ(inventory_hal_test::state.files.at(InventoryPublication::PATHS), oldPaths);
  scan.errorAt = 99;
  scan.failClose = true;
  EXPECT_EQ(build.build(scan, generation(), revision), InventoryPublicationResult::IoError);
  EXPECT_EQ(inventory_hal_test::state.files.at(InventoryPublication::INDEX), oldIndex);
  scan.failClose = false;
  ASSERT_EQ(build.build(scan, generation(), revision), InventoryPublicationResult::Ok);
  EXPECT_EQ(revision, 4u);
}
TEST_F(InventoryBuildTest, FailedReservationAndInvalidGenerationNeverBeginScan) {
  uint64_t revision = 42;
  inventory_hal_test::state.failNvsWrite = true;
  EXPECT_EQ(build.build(scan, generation(), revision), InventoryPublicationResult::IoError);
  EXPECT_EQ(revision, 42u);
  EXPECT_EQ(scan.begins, 0u);
  EXPECT_FALSE(inventory_hal_test::state.files.contains(InventoryPublication::INDEX_NEXT));
  EXPECT_EQ(build.build(scan, Identity{}, revision), InventoryPublicationResult::Invalid);
  EXPECT_EQ(scan.begins, 0u);
}
TEST_F(InventoryBuildTest, InterruptedPublicationIsRecoveredBeforeNextScanReservesRevision) {
  uint64_t revision = 42;
  inventory_hal_test::state.failRename = 2;
  EXPECT_EQ(build.build(scan, generation(), revision), InventoryPublicationResult::IoError);
  EXPECT_EQ(revision, 42u);
  inventory_hal_test::state.failRename = 0;
  ASSERT_EQ(build.build(scan, generation(), revision), InventoryPublicationResult::Ok);
  EXPECT_EQ(revision, 2u);
  EXPECT_EQ(validator.pair(InventoryPublication::INDEX, InventoryPublication::PATHS, generation(), revision),
            InventoryValidation::Valid);
}

namespace {}  // namespace
TEST(HalInventoryBaseResolverTest, ClassifiesFilesWithoutLosingUnsupportedContentOrMutatingMetadata) {
  auto& state = inventory_hal_test::state;
  state = {};
  HalInventoryBaseResolver resolver;
  ContentManifest metadata;
  metadata.length = 99;
  const auto unchanged = metadata;
  HalFile file;
  EXPECT_EQ(resolver.resolve("/book.epub", file, metadata), InventoryFileDecision::Error);
  state.files["/book.EPUB"] = {1};
  ASSERT_TRUE(Storage.openFileForReadReusing("COMPANION", "/book.EPUB", file));
  ASSERT_EQ(resolver.resolve("/book.EPUB", file, metadata), InventoryFileDecision::Include);
  EXPECT_EQ(metadata.kind, ContentKind::Epub);
  EXPECT_EQ(metadata.formatVersion, 1u);
  EXPECT_EQ(metadata.length, 0u);
  for (const auto path : {"/course.pack", "/font.TTF", "/font.cpfont", "/dictionary.dict.dz", "/dictionary.ifo"}) {
    metadata = unchanged;
    EXPECT_EQ(resolver.resolve(path, file, metadata), InventoryFileDecision::Error);
    EXPECT_EQ(metadata, unchanged);
  }
  metadata = unchanged;
  EXPECT_EQ(resolver.resolve("/note.txt", file, metadata), InventoryFileDecision::Skip);
  EXPECT_EQ(resolver.resolve("/.crosspoint/cache.epub", file, metadata), InventoryFileDecision::Skip);
  EXPECT_EQ(resolver.resolve("/.CrossPoint/cache.EPUB", file, metadata), InventoryFileDecision::Skip);
  EXPECT_EQ(resolver.resolve("/.crosspoint-other/book.epub", file, metadata), InventoryFileDecision::Include);
  metadata = unchanged;
  EXPECT_EQ(resolver.resolve("/books/../book.epub", file, metadata), InventoryFileDecision::Error);
  EXPECT_EQ(metadata, unchanged);
  ASSERT_TRUE(file.close());
  state.directories["/books"] = {};
  ASSERT_TRUE(Storage.openFileForReadReusing("COMPANION", "/books", file));
  EXPECT_EQ(resolver.resolve("/books", file, metadata), InventoryFileDecision::Include);
  EXPECT_EQ(resolver.resolve("/.crosspoint", file, metadata), InventoryFileDecision::Skip);
  EXPECT_EQ(resolver.resolve("/.CROSSPOINT", file, metadata), InventoryFileDecision::Skip);
  EXPECT_EQ(resolver.resolve("/.crosspoint-other", file, metadata), InventoryFileDecision::Include);
  EXPECT_EQ(metadata, unchanged);
}
TEST_F(InventoryBuildTest, ComposedSessionScansBooksFontsAndDictionaryWithoutOmittingKinds) {
  auto& state = inventory_hal_test::state;
  const auto load = [](const char* path) {
    std::ifstream file(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
  };
  state.directories["/"] = {{"books", true}, {"fonts", true}, {"dictionaries", true}, {".crosspoint", true}};
  state.directories["/books"] = {{"book.EPUB", false}, {"notes.txt", false}};
  state.files["/books/book.EPUB"] = {1, 2, 3};
  state.files["/books/notes.txt"] = {4};
  state.directories["/fonts"] = {{"font.cpfont", false}};
  state.files["/fonts/font.cpfont"] = load(FONT_FIXTURE);
  ASSERT_FALSE(state.files["/fonts/font.cpfont"].empty());
  state.directories["/dictionaries"] = {{"demo", true}};
  state.directories["/dictionaries/demo"] = {{"stem.dict", false}, {"stem.idx", false}, {"stem.ifo", false}};
  state.files["/dictionaries/demo/stem.dict"] = {'o', 'n', 'e', 't', 'w', 'o'};
  const auto indexPath = std::string(VECTOR_FIXTURE_DIR) + "DictionaryIndex-definitions.fixture";
  state.files["/dictionaries/demo/stem.idx"] = load(indexPath.c_str());
  const std::string info = "StarDict's dict ifo file\nversion=3.0.0\nbookname=Demo\nwordcount=2\nidxfilesize=24\n";
  state.files["/dictionaries/demo/stem.ifo"] = {info.begin(), info.end()};
  auto decoder = makeUniqueNoThrow<tinfl_decompressor>();
  auto window = makeUniqueNoThrow<uint8_t[]>(32768);
  ASSERT_TRUE(decoder);
  ASSERT_TRUE(window);
  auto resolver = makeUniqueNoThrow<HalInventoryResolverSession>(storage, std::span(scratch), *decoder,
                                                                 std::span(window.get(), 32768));
  ASSERT_TRUE(resolver);
  HalFile unopened;
  ContentManifest ignored;
  EXPECT_EQ(resolver->resolve("/books/book.EPUB", unopened, ignored), InventoryFileDecision::Error);
  ASSERT_TRUE(resolver->prepareAfterRecovery());
  auto session = makeUniqueNoThrow<HalInventoryBuildSession>(storage, *resolver, std::span(scratch));
  ASSERT_TRUE(session);
  uint64_t revision = 0;
  ASSERT_EQ(session->build(generation(), revision), InventoryPublicationResult::Ok);
  HalInventoryIndexStorage records;
  ASSERT_TRUE(records.open(InventoryPublication::INDEX));
  IndexedInventoryCatalog catalog(records, std::span(scratch));
  ASSERT_TRUE(catalog.open(generation()));
  ASSERT_EQ(catalog.count(), 3u);
  unsigned kinds = 0;
  for (unsigned at = 0; at < 3; ++at) {
    ContentManifest manifest;
    ASSERT_TRUE(catalog.read(at, manifest));
    kinds |= 1u << static_cast<unsigned>(manifest.kind);
  }
  EXPECT_EQ(kinds, (1u << static_cast<unsigned>(ContentKind::Epub)) | (1u << static_cast<unsigned>(ContentKind::Font)) |
                       (1u << static_cast<unsigned>(ContentKind::Dictionary)));
  const auto published = state.files;
  auto undersized = makeUniqueNoThrow<HalInventoryResolverSession>(storage, std::span(scratch).first(100), *decoder,
                                                                   std::span(window.get(), 32768));
  ASSERT_TRUE(undersized);
  EXPECT_FALSE(undersized->prepareAfterRecovery());
  EXPECT_EQ(state.files, published);
}
TEST_F(InventoryBuildTest, SessionOwnsRealTraversalHashingAndDisjointWorkspacePipeline) {
  auto& state = inventory_hal_test::state;
  state.directories["/"] = {{"books", true}, {".CrossPoint", true}};
  state.directories["/.CrossPoint"] = {{"private.epub", false}};
  state.files["/.CrossPoint/private.epub"] = {9};
  state.directories["/books"] = {{"copy.epub", false}, {"first.epub", false}, {"other.epub", false}};
  state.files["/books/first.epub"] = std::vector<uint8_t>(20000, 42);
  state.files["/books/copy.epub"] = state.files["/books/first.epub"];
  state.files["/books/other.epub"] = {1, 2, 3};
  HalInventoryBaseResolver resolver;
  auto session = makeUniqueNoThrow<HalInventoryBuildSession>(storage, resolver, std::span(scratch));
  ASSERT_TRUE(session);
  uint64_t revision = 42;
  ASSERT_EQ(session->build(generation(), revision), InventoryPublicationResult::Ok);
  EXPECT_EQ(revision, 1u);
  HalInventoryIndexStorage reader;
  ASSERT_TRUE(reader.open(InventoryPublication::INDEX));
  IndexedInventoryCatalog catalog(reader, scratch);
  ASSERT_TRUE(catalog.open(generation()));
  EXPECT_EQ(catalog.count(), 2u);
  ContentManifest actual;
  ASSERT_TRUE(catalog.read(0, actual));
  Digest expected;
  uint64_t bytes = 0;
  HalFile first("/books/first.epub");
  ASSERT_TRUE(hashInventoryFile(first, scratch, bytes, expected));
  ASSERT_TRUE(first.close());
  ContentManifest other;
  ASSERT_TRUE(catalog.read(1, other));
  EXPECT_TRUE((actual.contentHash == expected && actual.length == 20000) ||
              (other.contentHash == expected && other.length == 20000));
  ASSERT_TRUE(reader.close());
  EXPECT_EQ(validator.pair(InventoryPublication::INDEX, InventoryPublication::PATHS, generation(), revision),
            InventoryValidation::Valid);
  const auto preparations = state.preparations;
  ASSERT_EQ(session->build(generation(), revision), InventoryPublicationResult::Ok);
  EXPECT_EQ(revision, 2u);
  EXPECT_EQ(state.preparations, preparations);
  const auto oldIndex = state.files.at(InventoryPublication::INDEX);
  state.directoryErrorPath = "/books";
  EXPECT_EQ(session->build(generation(), revision), InventoryPublicationResult::IoError);
  EXPECT_EQ(revision, 2u);
  EXPECT_EQ(state.files.at(InventoryPublication::INDEX), oldIndex);
}
TEST_F(InventoryBuildTest, SessionRejectsShortWorkspaceBeforeStorageOrTraversal) {
  HalInventoryBaseResolver resolver;
  auto session = makeUniqueNoThrow<HalInventoryBuildSession>(storage, resolver, std::span(scratch).first(100));
  ASSERT_TRUE(session);
  uint64_t revision = 42;
  EXPECT_EQ(session->build(generation(), revision), InventoryPublicationResult::Invalid);
  EXPECT_EQ(revision, 42u);
  EXPECT_TRUE(inventory_hal_test::state.revisionBytes.empty());
  EXPECT_EQ(inventory_hal_test::state.preparations, 0u);
  EXPECT_TRUE(inventory_hal_test::state.files.empty());
}

TEST_F(InventoryBuildTest, SessionPartitionsRemainDisjointAcrossMultipleSortRuns) {
  auto& state = inventory_hal_test::state;
  state.directories["/"] = {{"books", true}};
  auto& entries = state.directories["/books"];
  entries.reserve(120);
  for (unsigned i = 0; i < 120; ++i) {
    const auto name = std::to_string(i) + ".epub";
    entries.push_back({name, false});
    state.files["/books/" + name] = {static_cast<uint8_t>(i), 42, 91};
  }
  HalInventoryBaseResolver resolver;
  auto session = makeUniqueNoThrow<HalInventoryBuildSession>(storage, resolver, std::span(scratch));
  ASSERT_TRUE(session);
  uint64_t revision = 0;
  ASSERT_EQ(session->build(generation(), revision), InventoryPublicationResult::Ok);
  HalInventoryIndexStorage reader;
  ASSERT_TRUE(reader.open(InventoryPublication::INDEX));
  IndexedInventoryCatalog catalog(reader, scratch);
  ASSERT_TRUE(catalog.open(generation()));
  EXPECT_EQ(catalog.count(), 120u);
  ASSERT_TRUE(reader.close());
  EXPECT_EQ(validator.pair(InventoryPublication::INDEX, InventoryPublication::PATHS, generation(), revision),
            InventoryValidation::Valid);
  EXPECT_GT(state.yields, 120u);
}

TEST_F(InventoryBuildTest, CourseDisabledResolverRejectsActivePackAndDelegatesOtherContent) {
  HalInventoryBaseResolver delegate;
  HalInventoryCourseResolver resolver(delegate);
  HalFile file(ACTIVE_COURSE_PATH);
  ContentManifest metadata;
  metadata.formatVersion = 45;
  EXPECT_EQ(resolver.resolve(ACTIVE_COURSE_PATH, file, metadata), InventoryFileDecision::Error);
  EXPECT_EQ(metadata.formatVersion, 45u);
  EXPECT_FALSE(resolver.verifyHashed(ACTIVE_COURSE_PATH, metadata));
  EXPECT_EQ(resolver.resolve("/books/book.epub", file, metadata), InventoryFileDecision::Include);
  EXPECT_TRUE(resolver.verifyHashed("/books/book.epub", metadata));
  EXPECT_TRUE(inventory_hal_test::state.files.empty());
}

TEST_F(InventoryBuildTest, BitmapFontResolverValidatesBorrowedFileAndRejectsCorruptRescan) {
  std::ifstream input(FONT_FIXTURE, std::ios::binary);
  std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  ASSERT_EQ(bytes.size(), 142u);
  auto& state = inventory_hal_test::state;
  state.directories["/"] = {{".fonts", true}};
  state.directories["/.fonts"] = {{"Demo", true}};
  state.directories["/.fonts/Demo"] = {{"Demo_16.cpfont", false}};
  state.files["/.fonts/Demo/Demo_16.cpfont"] = bytes;
  HalInventoryBaseResolver delegate;
  HalInventoryFontResolver resolver(delegate, std::span(scratch).first(HalInventoryBuildSession::HASH_SIZE));
  auto session = makeUniqueNoThrow<HalInventoryBuildSession>(storage, resolver, std::span(scratch));
  ASSERT_TRUE(session);
  uint64_t revision = 0;
  ASSERT_EQ(session->build(generation(), revision), InventoryPublicationResult::Ok);
  HalInventoryIndexStorage reader;
  ASSERT_TRUE(reader.open(InventoryPublication::INDEX));
  IndexedInventoryCatalog catalog(reader, scratch);
  ASSERT_TRUE(catalog.open(generation()));
  ASSERT_EQ(catalog.count(), 1u);
  ContentManifest manifest;
  ASSERT_TRUE(catalog.read(0, manifest));
  EXPECT_EQ(manifest.kind, ContentKind::Font);
  EXPECT_EQ(manifest.formatVersion, 4u);
  EXPECT_EQ(manifest.length, bytes.size());
  EXPECT_EQ(manifest.logicalIdentity, Identity{});
  ASSERT_TRUE(reader.close());
  const auto oldIndex = state.files.at(InventoryPublication::INDEX);
  state.files["/.fonts/Demo/Demo_16.cpfont"][126] = 0;
  EXPECT_EQ(session->build(generation(), revision), InventoryPublicationResult::IoError);
  EXPECT_EQ(revision, 1u);
  EXPECT_EQ(state.files.at(InventoryPublication::INDEX), oldIndex);
}

TEST_F(InventoryBuildTest, VectorFontResolverInventoriesUppercaseExtensionsAndRejectsBadChecksum) {
  auto& state = inventory_hal_test::state;
  state.directories["/"] = {{"fonts", true}};
  state.directories["/fonts"] = {{"Demo", true}};
  auto& entries = state.directories["/fonts/Demo"];
  entries.reserve(3);
  for (const auto& pair :
       {std::pair{"VectorFont-sfnt.fixture", "Demo.TTF"}, std::pair{"VectorFont-otto.fixture", "Demo.OTF"},
        std::pair{"VectorFont-collection.fixture", "Demo.TTC"}}) {
    std::ifstream input(std::string(VECTOR_FIXTURE_DIR) + pair.first, std::ios::binary);
    auto& bytes = state.files[std::string("/fonts/Demo/") + pair.second];
    bytes = {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    ASSERT_GT(bytes.size(), 140u);
    entries.push_back({pair.second, false});
  }
  HalInventoryBaseResolver delegate;
  HalInventoryFontResolver resolver(delegate, std::span(scratch).first(HalInventoryBuildSession::HASH_SIZE));
  auto session = makeUniqueNoThrow<HalInventoryBuildSession>(storage, resolver, std::span(scratch));
  ASSERT_TRUE(session);
  uint64_t revision = 0;
  ASSERT_EQ(session->build(generation(), revision), InventoryPublicationResult::Ok);
  HalInventoryIndexStorage reader;
  ASSERT_TRUE(reader.open(InventoryPublication::INDEX));
  IndexedInventoryCatalog catalog(reader, scratch);
  ASSERT_TRUE(catalog.open(generation()));
  ASSERT_EQ(catalog.count(), 3u);
  for (unsigned index = 0; index < 3; ++index) {
    ContentManifest manifest;
    ASSERT_TRUE(catalog.read(index, manifest));
    EXPECT_EQ(manifest.kind, ContentKind::Font);
    EXPECT_EQ(manifest.formatVersion, HalInventoryFontResolver::FORMAT_VECTOR);
  }
  ASSERT_TRUE(reader.close());
  const auto oldIndex = state.files.at(InventoryPublication::INDEX);
  state.files["/fonts/Demo/Demo.TTF"].back() ^= 1;
  EXPECT_EQ(session->build(generation(), revision), InventoryPublicationResult::IoError);
  EXPECT_EQ(revision, 1u);
  EXPECT_EQ(state.files.at(InventoryPublication::INDEX), oldIndex);
}
