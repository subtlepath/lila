#include <Memory.h>
#include <PersistableStore.h>
#include <gtest/gtest.h>

#include "HalBookmarkJsonStage.h"
#include "HalBookmarkLegacyBackup.h"
#include "HalBookmarkMigrationBackupSession.h"
#include "HalBookmarkMigrationRecord.h"
#include "HalBookmarkPreparationRecord.h"
#include "HalBookmarkPreparationStorage.h"
#include "HalBookmarkPublicationPaths.h"
#include "HalBookmarkPublicationRecord.h"
#include "HalBookmarkPublicationStorage.h"
#include "lib/Companion/CompanionBookmarkJsonWriter.h"
#include "lib/Companion/CompanionReadingBody.h"
#include "src/CompanionBookmarkChoicePage.h"
#include "src/CompanionBookmarkMigrationSession.h"
#include "src/CompanionBookmarkPreparationSession.h"
#include "src/CompanionBookmarkPublicationSession.h"
#include "src/CompanionBookmarkReaderSession.h"
#include "src/util/BookmarkFile.h"
#include "src/util/BookmarkPageMatch.h"
#include "src/util/BookmarkUtil.h"
#include "src/util/ProgressSaveDebounce.h"

namespace {
constexpr const char* BOOK = "/books/example.epub";
class JsonSink final : public companion::BookmarkJsonSink {
 public:
  JsonSink() { bytes.reserve(8192); }
  bool write(std::span<const uint8_t> value) override {
    ++calls;
    maxChunk = std::max(maxChunk, value.size());
    if (calls == failAt) return false;
    bytes.insert(bytes.end(), value.begin(), value.end());
    return true;
  }
  std::vector<uint8_t> bytes;
  unsigned calls = 0, failAt = 0;
  size_t maxChunk = 0;
};
class BookmarkFileTest : public ::testing::Test {
 protected:
  void SetUp() override {
    inventory_hal_test::state = {};
    bookmark_test::failRead = bookmark_test::failWrite = false;
    bookmark_test::writes = 0;
  }
  void install(std::string_view json) {
    inventory_hal_test::state.files[BookmarkUtil::getBookmarkPath(BOOK)] =
        std::vector<uint8_t>(json.begin(), json.end());
  }
};
TEST_F(BookmarkFileTest, StableIdsSurviveSerializationAndRename) {
  std::vector<BookmarkEntry> entries(2);
  entries[0].identity[0] = 3;
  entries[1].identity[15] = 9;
  entries[0].name = "First";
  entries[0].summary = "Summary";
  entries[0].computedSpineIndex = 5;
  entries[0].visibleTextOffset = 1000;
  entries[0].hasVisibleTextOffset = true;
  ASSERT_TRUE(BookmarkFile::save(BOOK, entries));
  std::vector<BookmarkEntry> loaded;
  ASSERT_TRUE(BookmarkFile::load(BOOK, loaded));
  ASSERT_EQ(loaded.size(), 2u);
  EXPECT_EQ(loaded[0].identity, entries[0].identity);
  EXPECT_EQ(loaded[1].identity, entries[1].identity);
  EXPECT_EQ(loaded[0].name, "First");
  EXPECT_EQ(loaded[0].summary, "Summary");
  EXPECT_EQ(loaded[0].computedSpineIndex, 5);
  EXPECT_TRUE(loaded[0].hasVisibleTextOffset);
  EXPECT_EQ(loaded[0].visibleTextOffset, 1000u);
  loaded[0].name = "Renamed";
  ASSERT_TRUE(BookmarkFile::save(BOOK, loaded));
  ASSERT_TRUE(BookmarkFile::load(BOOK, loaded));
  EXPECT_EQ(loaded[0].identity, entries[0].identity);
  EXPECT_EQ(loaded[0].name, "Renamed");
}
TEST(ProgressSaveDebounceTest, LatestRenderedAnchorIsSavedAfterIntervalAndReturnCancelsPending) {
  ProgressSaveDebounce save;
  const ProgressSaveDebounce::Position first{1, 0, 10, 100};
  save.stage(first);
  ASSERT_TRUE(save.due(0));
  save.complete(100, true);
  save.stage({1, 1, 10, 200});
  save.stage({2, 3, 20, 900});
  EXPECT_FALSE(save.due(5099));
  ASSERT_TRUE(save.due(5100));
  EXPECT_EQ(save.position().spine, 2);
  EXPECT_EQ(save.position().offset, 900u);
  save.stage(first);
  EXPECT_FALSE(save.hasPending());
  EXPECT_FALSE(save.due(10000));
}
TEST(ProgressSaveDebounceTest, FailedWritesRemainPendingAndRetryIsBoundedAcrossClockWrap) {
  ProgressSaveDebounce save;
  save.stage({1, 2, 10, 345});
  save.complete(UINT32_MAX - 1000, false);
  EXPECT_TRUE(save.hasPending());
  EXPECT_FALSE(save.due(3998));
  EXPECT_TRUE(save.due(3999));
  save.stage({2, 0, 11, 0});
  EXPECT_EQ(save.position().offset, 0u);
  save.complete(4000, true);
  EXPECT_FALSE(save.hasPending());
}
TEST(ProgressSaveDebounceTest, OffsetChangesAreRetainedAndForcedSaveBypassesDuplicateSuppression) {
  ProgressSaveDebounce save;
  const ProgressSaveDebounce::Position position{1, 2, 10, 123};
  save.stage(position);
  save.complete(1, true);
  save.stage({1, 2, 10, 124});
  EXPECT_TRUE(save.hasPending());
  EXPECT_EQ(save.position().offset, 124u);
  save.stage(position);
  EXPECT_FALSE(save.hasPending());
  save.stage(position, true);
  EXPECT_TRUE(save.hasPending());
  EXPECT_FALSE(save.due(2));
  EXPECT_EQ(save.position(), position);
  save.clearPending();
  EXPECT_FALSE(save.hasPending());
}
TEST(ProgressSaveDebounceTest, UncertainWriteInvalidatesSavedPositionBeforeReturningToIt) {
  ProgressSaveDebounce save;
  const ProgressSaveDebounce::Position original{1, 0, 10, 100};
  save.stage(original);
  save.complete(100, true);
  save.stage({1, 1, 10, 200});
  save.complete(5100, false);
  save.stage(original);
  ASSERT_TRUE(save.hasPending());
  EXPECT_EQ(save.position(), original);
  EXPECT_FALSE(save.due(10099));
  EXPECT_TRUE(save.due(10100));
  save.complete(10100, true);
  save.stage(original);
  EXPECT_FALSE(save.hasPending());
}
TEST_F(BookmarkFileTest, LegacyEntriesRemainReadableWithoutInventedIdentities) {
  install(R"({"bookmarks":[{"name":"Old","vo":42},{"name":"Another"}]})");
  std::vector<BookmarkEntry> entries;
  ASSERT_TRUE(BookmarkFile::load(BOOK, entries));
  ASSERT_EQ(entries.size(), 2u);
  EXPECT_FALSE(BookmarkIdentity::valid(entries[0].identity));
  EXPECT_FALSE(BookmarkIdentity::valid(entries[1].identity));
  ASSERT_TRUE(BookmarkFile::save(BOOK, entries));
  JsonDocument doc;
  ASSERT_TRUE(PersistableStoreBase::readDocFromFile(BookmarkUtil::getBookmarkPath(BOOK).c_str(), doc));
  EXPECT_TRUE(doc["bookmarks"][0]["id"].isNull());
  EXPECT_TRUE(doc["bookmarks"][1]["id"].isNull());
}
TEST_F(BookmarkFileTest, MalformedIdentitiesRejectWholeLoadAndLeaveFileUntouched) {
  for (const auto id : {"null", "4", "\"00000000000000000000000000000000\"",
                        "\"03000000000000000000000000000000\\u0000hidden\"", "\"bad\""}) {
    install(std::string("{\"bookmarks\":[{}, {\"id\":") + id + "}]}");
    const auto before = inventory_hal_test::state.files;
    std::vector<BookmarkEntry> entries;
    EXPECT_FALSE(BookmarkFile::load(BOOK, entries));
    EXPECT_TRUE(entries.empty());
    EXPECT_EQ(inventory_hal_test::state.files, before);
    EXPECT_EQ(bookmark_test::writes, 0u);
  }
}
TEST_F(BookmarkFileTest, DuplicateIdentitiesRejectLoadAndSave) {
  install(R"({"bookmarks":[{"id":"03000000000000000000000000000000"},{"id":"03000000000000000000000000000000"}]})");
  const auto before = inventory_hal_test::state.files;
  std::vector<BookmarkEntry> entries;
  EXPECT_FALSE(BookmarkFile::load(BOOK, entries));
  EXPECT_TRUE(entries.empty());
  entries.resize(2);
  entries[0].identity[0] = entries[1].identity[0] = 3;
  EXPECT_FALSE(BookmarkFile::save(BOOK, entries));
  EXPECT_EQ(inventory_hal_test::state.files, before);
  EXPECT_EQ(bookmark_test::writes, 0u);
}
TEST_F(BookmarkFileTest, StorageErrorsPropagate) {
  std::vector<BookmarkEntry> entries(1);
  entries[0].identity[0] = 7;
  ASSERT_TRUE(BookmarkFile::save(BOOK, entries));
  const auto before = inventory_hal_test::state.files;
  bookmark_test::failWrite = true;
  entries[0].name = "Changed";
  EXPECT_FALSE(BookmarkFile::save(BOOK, entries));
  EXPECT_EQ(inventory_hal_test::state.files, before);
  bookmark_test::failRead = true;
  EXPECT_FALSE(BookmarkFile::load(BOOK, entries));
  EXPECT_TRUE(entries.empty());
}
TEST_F(BookmarkFileTest, MalformedArrayAndEntriesRejectWholeLoad) {
  for (const auto json :
       {"{}", "{\"bookmarks\":null}", "{\"bookmarks\":{}}", "{\"bookmarks\":[{},null]}", "{\"bookmarks\":[{},4]}"}) {
    install(json);
    const auto before = inventory_hal_test::state.files;
    std::vector<BookmarkEntry> entries;
    EXPECT_FALSE(BookmarkFile::load(BOOK, entries));
    EXPECT_TRUE(entries.empty());
    EXPECT_EQ(inventory_hal_test::state.files, before);
  }
  install(R"({"bookmarks":[]})");
  std::vector<BookmarkEntry> entries;
  EXPECT_TRUE(BookmarkFile::load(BOOK, entries));
  EXPECT_TRUE(entries.empty());
}

TEST_F(BookmarkFileTest, StreamedJsonLoadsPortableAnchorsEscapedTextAndOmitsDeletion) {
  using namespace companion;
  JsonSink sink;
  BookmarkJsonWriter writer(sink);
  ASSERT_TRUE(writer.begin());
  BookmarkBodyView bookmark;
  bookmark.identity[0] = 3;
  bookmark.anchor = {65535, UINT32_MAX};
  const std::string name = "Quote\" slash\\ tab\t newline\n control\x01";
  const std::string summary = "Espa\xc3\xb1ol / \xe6\x97\xa5\xe6\x9c\xac";
  bookmark.name = {reinterpret_cast<const uint8_t*>(name.data()), name.size()};
  bookmark.summary = {reinterpret_cast<const uint8_t*>(summary.data()), summary.size()};
  std::array<uint8_t, MAX_BOOKMARK_BODY_SIZE> body{};
  const auto length = encodeBookmarkBody(bookmark, body);
  ASSERT_NE(length, 0u);
  ASSERT_TRUE(writer.append(std::span(body).first(length)));
  bookmark.identity[0] = 7;
  bookmark.deleted = true;
  ASSERT_TRUE(writer.append(std::span(body).first(encodeBookmarkBody(bookmark, body))));
  ASSERT_TRUE(writer.finish());
  EXPECT_LE(sink.maxChunk, 128u);
  EXPECT_EQ(writer.bytesWritten(), sink.bytes.size());
  inventory_hal_test::state.files[BookmarkUtil::getBookmarkPath(BOOK)] = sink.bytes;
  std::vector<BookmarkEntry> entries;
  ASSERT_TRUE(BookmarkFile::load(BOOK, entries));
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].identity[0], 3);
  EXPECT_EQ(entries[0].computedSpineIndex, 65535);
  EXPECT_EQ(entries[0].visibleTextOffset, UINT32_MAX);
  EXPECT_TRUE(entries[0].hasVisibleTextOffset);
  EXPECT_EQ(entries[0].name, name);
  EXPECT_EQ(entries[0].summary, summary);
  EXPECT_FALSE(writer.finish());
}

TEST_F(BookmarkFileTest, StreamedJsonMaximumEscapesAndSinkFailureRemainBounded) {
  using namespace companion;
  BookmarkBodyView bookmark;
  bookmark.identity[0] = 3;
  const std::string name(128, '\x01'), summary(512, '\n');
  bookmark.name = {reinterpret_cast<const uint8_t*>(name.data()), name.size()};
  bookmark.summary = {reinterpret_cast<const uint8_t*>(summary.data()), summary.size()};
  std::array<uint8_t, MAX_BOOKMARK_BODY_SIZE> body{};
  ASSERT_EQ(encodeBookmarkBody(bookmark, body), body.size());
  JsonSink sink;
  BookmarkJsonWriter writer(sink);
  ASSERT_TRUE(writer.begin());
  ASSERT_TRUE(writer.append(body));
  ASSERT_TRUE(writer.finish());
  EXPECT_LE(sink.maxChunk, 128u);
  inventory_hal_test::state.files[BookmarkUtil::getBookmarkPath(BOOK)] = sink.bytes;
  std::vector<BookmarkEntry> entries;
  ASSERT_TRUE(BookmarkFile::load(BOOK, entries));
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].name, name);
  EXPECT_EQ(entries[0].summary, summary);
  JsonSink failed;
  failed.failAt = 2;
  BookmarkJsonWriter refused(failed);
  ASSERT_TRUE(refused.begin());
  EXPECT_FALSE(refused.append(body));
  EXPECT_FALSE(refused.finish());
  EXPECT_EQ(failed.calls, 2u);
  EXPECT_LE(failed.maxChunk, 128u);
}

TEST_F(BookmarkFileTest, StreamedJsonRejectsDuplicatesMalformedBodiesAndSupportsEmptyState) {
  using namespace companion;
  JsonSink sink;
  BookmarkJsonWriter writer(sink);
  ASSERT_TRUE(writer.begin());
  ASSERT_TRUE(writer.finish());
  inventory_hal_test::state.files[BookmarkUtil::getBookmarkPath(BOOK)] = sink.bytes;
  std::vector<BookmarkEntry> entries;
  EXPECT_TRUE(BookmarkFile::load(BOOK, entries));
  EXPECT_TRUE(entries.empty());
  JsonSink invalid;
  BookmarkJsonWriter refused(invalid);
  ASSERT_TRUE(refused.begin());
  std::array<uint8_t, 18> body{};
  EXPECT_FALSE(refused.append(body));
  EXPECT_FALSE(refused.finish());
  EXPECT_TRUE(invalid.bytes.empty());
  JsonSink duplicate;
  BookmarkJsonWriter duplicated(duplicate);
  ASSERT_TRUE(duplicated.begin());
  BookmarkBodyView bookmark;
  bookmark.identity[0] = 3;
  bookmark.deleted = true;
  ASSERT_EQ(encodeBookmarkBody(bookmark, body), body.size());
  ASSERT_TRUE(duplicated.append(body));
  EXPECT_FALSE(duplicated.append(body));
  EXPECT_FALSE(duplicated.finish());
}
TEST_F(BookmarkFileTest, VerifiedJsonStageLoadsThroughNativeParserAndPreservesCanonicalUntilPublication) {
  using namespace companion;
  inventory_hal_test::state.enumerateFileMap = true;
  install(R"({"bookmarks":[{"name":"Local"}]})");
  const auto original = inventory_hal_test::state.files.at(BookmarkUtil::getBookmarkPath(BOOK));
  std::array<uint8_t, 128> scratch{};
  auto stage = makeUniqueNoThrow<HalBookmarkJsonStage>(scratch);
  ASSERT_TRUE(stage);
  ASSERT_TRUE(stage->begin(2));
  BookmarkBodyView bookmark;
  bookmark.identity[0] = 3;
  bookmark.anchor = {2, 123};
  std::array<uint8_t, MAX_BOOKMARK_BODY_SIZE> body{};
  ASSERT_TRUE(stage->append(std::span(body).first(encodeBookmarkBody(bookmark, body))));
  bookmark.identity[0] = 7;
  bookmark.deleted = true;
  ASSERT_TRUE(stage->append(std::span(body).first(encodeBookmarkBody(bookmark, body))));
  ASSERT_TRUE(stage->finish());
  EXPECT_TRUE(stage->isSealed());
  EXPECT_EQ(inventory_hal_test::state.files.at(BookmarkUtil::getBookmarkPath(BOOK)), original);
  const auto json = inventory_hal_test::state.files.at(HalBookmarkJsonStage::PATH);
  EXPECT_EQ(stage->bytesWritten(), json.size());
  install(std::string_view(reinterpret_cast<const char*>(json.data()), json.size()));
  std::vector<BookmarkEntry> entries;
  ASSERT_TRUE(BookmarkFile::load(BOOK, entries));
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].identity[0], 3);
  EXPECT_EQ(entries[0].computedSpineIndex, 2);
  EXPECT_EQ(entries[0].visibleTextOffset, 123u);
  EXPECT_TRUE(stage->cleanup());
  EXPECT_FALSE(inventory_hal_test::state.files.count(HalBookmarkJsonStage::PATH));
}

TEST_F(BookmarkFileTest, JsonStageRejectsIncompleteInputForeignCandidatesAndFailedSync) {
  using namespace companion;
  auto& state = inventory_hal_test::state;
  state.enumerateFileMap = true;
  std::array<uint8_t, 128> scratch{};
  auto stage = makeUniqueNoThrow<HalBookmarkJsonStage>(scratch);
  ASSERT_TRUE(stage);
  ASSERT_TRUE(stage->begin(1));
  EXPECT_FALSE(stage->finish());
  EXPECT_FALSE(stage->isSealed());
  EXPECT_TRUE(stage->cleanup());
  stage.reset();
  state.files[HalBookmarkJsonStage::PATH] = {1, 2, 3};
  stage = makeUniqueNoThrow<HalBookmarkJsonStage>(scratch);
  ASSERT_TRUE(stage);
  EXPECT_FALSE(stage->begin(0));
  stage.reset();
  EXPECT_EQ(state.files.at(HalBookmarkJsonStage::PATH), (std::vector<uint8_t>{1, 2, 3}));
  state.files.erase(HalBookmarkJsonStage::PATH);
  stage = makeUniqueNoThrow<HalBookmarkJsonStage>(scratch);
  ASSERT_TRUE(stage);
  ASSERT_TRUE(stage->begin(0));
  state.failSyncPath = HalBookmarkJsonStage::PATH;
  EXPECT_FALSE(stage->finish());
  EXPECT_FALSE(stage->isSealed());
  state.failSyncPath.clear();
  EXPECT_TRUE(stage->cleanup());
  EXPECT_FALSE(state.files.count(HalBookmarkJsonStage::PATH));
}
}  // namespace

TEST_F(BookmarkFileTest, PublicationRecordsAreVerifiedImmutableAndRecoverTemporary) {
  using namespace companion;
  constexpr const char* path = "/.crosspoint/companion/bookmark-intent";
  constexpr const char* temporary = "/.crosspoint/companion/bookmark-intent-next";
  ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
  auto store = makeUniqueNoThrow<HalBookmarkPublicationRecord>();
  ASSERT_TRUE(store);
  BookmarkPublicationClaim claim;
  claim.transaction.fill(1);
  claim.storageGeneration.fill(2);
  claim.edition.fill(3);
  claim.frontier.fill(4);
  claim.candidateHash.fill(5);
  claim.candidateLength = 50;
  claim.recordCount = 1;
  claim.recordSize = 512;
  EXPECT_EQ(store->inspect(path, claim), BookmarkPublicationRecord::Missing);
  std::array<uint8_t, BOOKMARK_PUBLICATION_RECORD_SIZE> bytes{};
  ASSERT_TRUE(encodeBookmarkPublicationRecord(claim, bytes));
  inventory_hal_test::state.files[temporary] = std::vector<uint8_t>(bytes.begin(), bytes.end());
  ASSERT_TRUE(store->persist(path, temporary, claim));
  EXPECT_EQ(store->inspect(path, claim), BookmarkPublicationRecord::Matches);
  EXPECT_FALSE(inventory_hal_test::state.files.contains(temporary));
  const auto before = inventory_hal_test::state.files;
  EXPECT_TRUE(store->persist(path, temporary, claim));
  auto foreign = claim;
  foreign.transaction[0] = 9;
  EXPECT_FALSE(store->persist(path, temporary, foreign));
  EXPECT_FALSE(store->remove(path, foreign));
  EXPECT_EQ(inventory_hal_test::state.files, before);
  ASSERT_TRUE(store->remove(path, claim));
  EXPECT_EQ(store->inspect(path, claim), BookmarkPublicationRecord::Missing);
  inventory_hal_test::state.files[temporary] = {1, 2, 3};
  const auto partial = inventory_hal_test::state.files;
  EXPECT_FALSE(store->persist(path, temporary, claim));
  EXPECT_EQ(inventory_hal_test::state.files, partial);
}

TEST_F(BookmarkFileTest, PublicationRecordRetryRequiresSyncAndPreservesReadOutputOnFailure) {
  using namespace companion;
  constexpr const char* path = "/.crosspoint/companion/bookmark-intent";
  constexpr const char* temporary = "/.crosspoint/companion/bookmark-intent-next";
  ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
  auto store = makeUniqueNoThrow<HalBookmarkPublicationRecord>();
  ASSERT_TRUE(store);
  BookmarkPublicationClaim claim;
  claim.transaction.fill(1);
  claim.storageGeneration.fill(2);
  claim.edition.fill(3);
  claim.frontier.fill(4);
  claim.candidateHash.fill(5);
  claim.candidateLength = 50;
  claim.recordCount = 1;
  claim.recordSize = 512;
  inventory_hal_test::state.failSync = true;
  EXPECT_FALSE(store->persist(path, temporary, claim));
  EXPECT_FALSE(store->persist(path, temporary, claim));
  EXPECT_FALSE(inventory_hal_test::state.files.contains(path));
  inventory_hal_test::state.failSync = false;
  ASSERT_TRUE(store->persist(path, temporary, claim));
  auto output = claim;
  output.transaction[0] = 9;
  const auto unchanged = output;
  inventory_hal_test::state.failClosePath = path;
  EXPECT_EQ(store->read(path, output), BookmarkPublicationRecord::Error);
  EXPECT_EQ(output, unchanged);
  inventory_hal_test::state.failClosePath.clear();
  inventory_hal_test::state.files[path][10] ^= 1;
  EXPECT_EQ(store->read(path, output), BookmarkPublicationRecord::Other);
  EXPECT_EQ(output, unchanged);
}

TEST_F(BookmarkFileTest, PublicationInstallsVerifiedCandidateAndRecoversLostRenameAcknowledgement) {
  using namespace companion;
  const BookmarkPublicationPaths paths{
      "/.crosspoint/companion/bookmark-active",       HalBookmarkJsonStage::PATH,
      "/.crosspoint/companion/bookmark-backup",       "/.crosspoint/companion/bookmark-intent",
      "/.crosspoint/companion/bookmark-intent-next",  "/.crosspoint/companion/bookmark-receipt",
      "/.crosspoint/companion/bookmark-receipt-next", TRANSFER_DIRECTORY};
  ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
  std::array<uint8_t, 128> scratch{};
  auto stage = makeUniqueNoThrow<HalBookmarkJsonStage>(scratch);
  ASSERT_TRUE(stage);
  ASSERT_TRUE(stage->begin(0));
  ASSERT_TRUE(stage->finish());
  BookmarkPublicationClaim claim;
  claim.transaction.fill(1);
  claim.storageGeneration.fill(2);
  claim.edition.fill(3);
  claim.frontier.fill(4);
  claim.candidateLength = stage->bytesWritten();
  claim.candidateHash = stage->contentHash();
  claim.recordCount = 1;
  claim.recordSize = 512;
  auto records = makeUniqueNoThrow<HalBookmarkPublicationRecord>();
  ASSERT_TRUE(records);
  EXPECT_FALSE(stage->retainForPublication(*records, paths.intent, claim));
  stage.reset();
  stage = makeUniqueNoThrow<HalBookmarkJsonStage>(scratch);
  ASSERT_TRUE(stage->begin(0));
  ASSERT_TRUE(stage->finish());
  ASSERT_TRUE(records->persist(paths.intent, paths.intentTemporary, claim));
  ASSERT_TRUE(stage->retainForPublication(*records, paths.intent, claim));
  stage.reset();
  ASSERT_TRUE(inventory_hal_test::state.files.contains(paths.candidate));
  const auto validator = +[](void*, const BookmarkPublicationClaim&) { return true; };
  auto storage = makeUniqueNoThrow<HalBookmarkPublicationStorage>(paths, scratch, validator, validator, nullptr);
  ASSERT_TRUE(storage);
  BookmarkPublication publication(*storage);
  inventory_hal_test::state.failRenameAfterSource = paths.candidate;
  EXPECT_EQ(publication.recover(claim), TintaJournalResult::IoError);
  inventory_hal_test::state.failRenameAfterSource.clear();
  EXPECT_EQ(publication.recover(claim), TintaJournalResult::Ok);
  EXPECT_EQ(storage->file(BookmarkPublicationRole::Active, claim), BookmarkPublicationFile::Candidate);
  EXPECT_EQ(storage->intent(claim), BookmarkPublicationRecord::Missing);
  EXPECT_EQ(storage->receipt(claim), BookmarkPublicationRecord::Matches);
  EXPECT_FALSE(inventory_hal_test::state.files.contains(paths.candidate));
}

TEST_F(BookmarkFileTest, PublicationPreservesOriginalUntilReceiptAndRefusesForeignBackup) {
  using namespace companion;
  const BookmarkPublicationPaths paths{
      "/.crosspoint/companion/bookmark-active",       HalBookmarkJsonStage::PATH,
      "/.crosspoint/companion/bookmark-backup",       "/.crosspoint/companion/bookmark-intent",
      "/.crosspoint/companion/bookmark-intent-next",  "/.crosspoint/companion/bookmark-receipt",
      "/.crosspoint/companion/bookmark-receipt-next", TRANSFER_DIRECTORY};
  ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
  std::array<uint8_t, 128> scratch{};
  auto stage = makeUniqueNoThrow<HalBookmarkJsonStage>(scratch);
  ASSERT_TRUE(stage->begin(0));
  ASSERT_TRUE(stage->finish());
  BookmarkPublicationClaim claim;
  claim.transaction.fill(1);
  claim.storageGeneration.fill(2);
  claim.edition.fill(3);
  claim.frontier.fill(4);
  claim.candidateLength = stage->bytesWritten();
  claim.candidateHash = stage->contentHash();
  claim.recordCount = 1;
  claim.recordSize = 512;
  claim.hadOriginal = true;
  const std::vector<uint8_t> old = {'o', 'l', 'd'};
  inventory_hal_test::state.files[paths.active] = old;
  auto original = Storage.open(paths.active, O_RDONLY);
  ASSERT_TRUE(hashInventoryFile(original, scratch, claim.originalLength, claim.originalHash));
  ASSERT_TRUE(original.close());
  auto records = makeUniqueNoThrow<HalBookmarkPublicationRecord>();
  ASSERT_TRUE(records->persist(paths.intent, paths.intentTemporary, claim));
  ASSERT_TRUE(stage->retainForPublication(*records, paths.intent, claim));
  stage.reset();
  const auto validator = +[](void*, const BookmarkPublicationClaim&) { return true; };
  auto storage = makeUniqueNoThrow<HalBookmarkPublicationStorage>(paths, scratch, validator, validator, nullptr);
  BookmarkPublication publication(*storage);
  inventory_hal_test::state.files[paths.backup] = {9};
  const auto foreign = inventory_hal_test::state.files;
  EXPECT_EQ(publication.recover(claim), TintaJournalResult::Corrupt);
  EXPECT_EQ(inventory_hal_test::state.files, foreign);
  inventory_hal_test::state.files.erase(paths.backup);
  inventory_hal_test::state.failRenameAfterSource = paths.active;
  EXPECT_EQ(publication.recover(claim), TintaJournalResult::IoError);
  EXPECT_EQ(inventory_hal_test::state.files.at(paths.backup), old);
  EXPECT_EQ(storage->receipt(claim), BookmarkPublicationRecord::Missing);
  inventory_hal_test::state.failRenameAfterSource.clear();
  EXPECT_EQ(publication.recover(claim), TintaJournalResult::Ok);
  EXPECT_EQ(storage->receipt(claim), BookmarkPublicationRecord::Matches);
  EXPECT_FALSE(inventory_hal_test::state.files.contains(paths.backup));
}

TEST_F(BookmarkFileTest, PublicationPathsAreStableScopedAndRejectInvalidActivePaths) {
  using namespace companion;
  BookmarkPublicationClaim claim;
  claim.transaction.fill(1);
  claim.storageGeneration.fill(2);
  claim.edition.fill(3);
  claim.frontier.fill(4);
  claim.candidateHash.fill(5);
  claim.candidateLength = 50;
  claim.recordCount = 1;
  claim.recordSize = 512;
  for (const auto invalid : {"/.crosspoint/books/a.json", "/.crosspoint/bookmarks/../a.json",
                             "/.crosspoint/bookmarks/a/b.json", "/.crosspoint/bookmarks", "/.crosspoint/bookmarks/",
                             "/.crosspoint/bookmarksX/a.json", "/.crosspoint/bookmarks/\xed\xa0\x80.json"}) {
    auto owner = makeUniqueNoThrow<HalBookmarkPublicationPaths>();
    ASSERT_TRUE(owner);
    EXPECT_FALSE(owner->initialize(invalid, claim));
    EXPECT_EQ(owner->paths().active, nullptr);
  }
  auto first = makeUniqueNoThrow<HalBookmarkPublicationPaths>();
  auto second = makeUniqueNoThrow<HalBookmarkPublicationPaths>();
  const auto active = BookmarkUtil::getBookmarkPath(BOOK);
  ASSERT_TRUE(first->initialize(active, claim));
  ASSERT_TRUE(first->matches(claim));
  const auto paths = first->paths();
  EXPECT_STREQ(paths.active, active.c_str());
  EXPECT_STREQ(paths.activeParent, HalBookmarkPublicationPaths::ACTIVE_PARENT);
  EXPECT_STREQ(paths.candidate, HalBookmarkJsonStage::PATH);
  EXPECT_STREQ(paths.receipt, "/.crosspoint/companion/bookmark-receipt-01010101010101010101010101010101");
  auto next = claim;
  next.transaction[0] = 2;
  EXPECT_FALSE(first->initialize(active, next));
  EXPECT_FALSE(first->matches(next));
  ASSERT_TRUE(second->initialize(active, next));
  EXPECT_STRNE(paths.receipt, second->paths().receipt);
  EXPECT_STRNE(paths.backup, second->paths().backup);
  EXPECT_STREQ(paths.intent, second->paths().intent);
  EXPECT_STREQ(paths.active, active.c_str());
  next = claim;
  next.edition[0] = 9;
  EXPECT_FALSE(first->matches(next));
  auto unicode = makeUniqueNoThrow<HalBookmarkPublicationPaths>();
  ASSERT_TRUE(unicode->initialize("/.crosspoint/bookmarks/café-📖.json", claim));
  EXPECT_STREQ(unicode->paths().active, "/.crosspoint/bookmarks/café-📖.json");
}

TEST_F(BookmarkFileTest, EditionPublicationPathsBindExactHashAndPreserveLegacyRecoveryPaths) {
  using namespace companion;
  BookmarkPublicationClaim claim;
  claim.transaction.fill(1);
  claim.storageGeneration.fill(2);
  claim.edition.fill(3);
  claim.frontier.fill(4);
  claim.candidateHash.fill(5);
  claim.candidateLength = 16;
  claim.recordSize = 512;
  std::array<char, 128> active{};
  ASSERT_TRUE(BookmarkUtil::getBookmarkEditionPath(claim.edition, active));
  auto paths = makeUniqueNoThrow<HalBookmarkPublicationPaths>();
  ASSERT_TRUE(paths);
  ASSERT_TRUE(paths->initialize(active.data(), claim));
  EXPECT_STREQ(paths->paths().activeParent, BOOKMARK_EDITION_CACHE_PARENT);
  EXPECT_STREQ(paths->paths().active, active.data());
  auto foreign = claim;
  foreign.edition[0] = 9;
  auto rejected = makeUniqueNoThrow<HalBookmarkPublicationPaths>();
  ASSERT_TRUE(rejected);
  EXPECT_FALSE(rejected->initialize(active.data(), foreign));
  EXPECT_EQ(rejected->paths().active, nullptr);
  const std::string canonical(active.data());
  for (const auto& invalid : {canonical + "/x", canonical + ".json",
                              std::string("/.crosspoint/bookmarks/editions/") + std::string(64, 'A') + ".json"}) {
    rejected = makeUniqueNoThrow<HalBookmarkPublicationPaths>();
    ASSERT_TRUE(rejected);
    EXPECT_FALSE(rejected->initialize(invalid, claim));
  }
  rejected = makeUniqueNoThrow<HalBookmarkPublicationPaths>();
  ASSERT_TRUE(rejected);
  EXPECT_TRUE(rejected->initialize("/.crosspoint/bookmarks/legacy.json", claim));
  EXPECT_STREQ(rejected->paths().activeParent, BOOKMARK_CACHE_PARENT);
}

TEST_F(BookmarkFileTest, UnicodePublicationPathAndFoldedHashesPreserveRecoveryBindings) {
  using namespace companion;
  ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
  BookmarkPreparationClaim claim;
  claim.transaction.fill(1);
  claim.storageGeneration.fill(2);
  claim.edition.fill(3);
  const auto validator = +[](void*, const BookmarkPreparationClaim&) { return true; };
  auto session = makeUniqueNoThrow<NativeBookmarkPreparationSession>(validator, nullptr);
  ASSERT_TRUE(session);
  ASSERT_EQ(
      session->begin("/.crosspoint/bookmarks/café-📖.json", claim.edition, claim.transaction, claim.storageGeneration),
      TintaJournalResult::Ok);
  inventory_hal_test::state.files[HalBookmarkJsonStage::PATH] = {1};
  const auto retained = inventory_hal_test::state.files;
  session.reset();
  session = makeUniqueNoThrow<NativeBookmarkPreparationSession>(validator, nullptr);
  EXPECT_EQ(session->recover("/.crosspoint/bookmarks/cafe\xcc\x81-📖.json", claim.edition, claim.storageGeneration),
            TintaJournalResult::Corrupt);
  EXPECT_EQ(inventory_hal_test::state.files, retained);
  session.reset();
  session = makeUniqueNoThrow<NativeBookmarkPreparationSession>(validator, nullptr);
  EXPECT_EQ(session->recover("/.crosspoint/bookmarks/CAFÉ-📖.JSON", claim.edition, claim.storageGeneration),
            TintaJournalResult::Ok);
  EXPECT_FALSE(inventory_hal_test::state.files.contains(HalBookmarkJsonStage::PATH));
}

TEST_F(BookmarkFileTest, ScopedReceiptsAllowSuccessivePublicationsForSameNativeBookmarkPath) {
  using namespace companion;
  inventory_hal_test::state.enumerateFileMap = true;
  ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
  ASSERT_TRUE(Storage.ensureDirectoryExists(HalBookmarkPublicationPaths::ACTIVE_PARENT));
  std::array<uint8_t, 128> scratch{};
  const auto active = BookmarkUtil::getBookmarkPath(BOOK);
  BookmarkPublicationClaim claim;
  claim.storageGeneration.fill(2);
  claim.edition.fill(3);
  claim.frontier.fill(4);
  claim.recordCount = 1;
  claim.recordSize = 512;
  std::string firstReceipt;
  for (uint8_t transaction = 1; transaction <= 2; ++transaction) {
    claim.transaction.fill(transaction);
    auto stage = makeUniqueNoThrow<HalBookmarkJsonStage>(scratch);
    ASSERT_TRUE(stage->begin(0));
    ASSERT_TRUE(stage->finish());
    claim.candidateHash = stage->contentHash();
    claim.candidateLength = stage->bytesWritten();
    if (transaction == 2) {
      claim.hadOriginal = true;
      auto file = Storage.open(active.c_str(), O_RDONLY);
      ASSERT_TRUE(hashInventoryFile(file, scratch, claim.originalLength, claim.originalHash));
      ASSERT_TRUE(file.close());
    }
    auto owner = makeUniqueNoThrow<HalBookmarkPublicationPaths>();
    ASSERT_TRUE(owner->initialize(active, claim));
    const auto paths = owner->paths();
    auto records = makeUniqueNoThrow<HalBookmarkPublicationRecord>();
    ASSERT_TRUE(records->persist(paths.intent, paths.intentTemporary, claim));
    ASSERT_TRUE(stage->retainForPublication(*records, paths.intent, claim));
    stage.reset();
    const auto context = +[](void* opaque, const BookmarkPublicationClaim& value) {
      return static_cast<HalBookmarkPublicationPaths*>(opaque)->matches(value);
    };
    const auto authority = +[](void*, const BookmarkPublicationClaim&) { return true; };
    auto storage = makeUniqueNoThrow<HalBookmarkPublicationStorage>(paths, scratch, context, authority, owner.get());
    BookmarkPublication publisher(*storage);
    ASSERT_EQ(publisher.publish(claim), TintaJournalResult::Ok);
    EXPECT_EQ(storage->receipt(claim), BookmarkPublicationRecord::Matches);
    EXPECT_EQ(storage->intent(claim), BookmarkPublicationRecord::Missing);
    EXPECT_EQ(storage->file(BookmarkPublicationRole::Active, claim), BookmarkPublicationFile::Candidate);
    if (transaction == 1)
      firstReceipt = paths.receipt;
    else
      EXPECT_TRUE(inventory_hal_test::state.files.contains(firstReceipt));
  }
  std::vector<BookmarkEntry> loaded;
  ASSERT_TRUE(BookmarkFile::load(BOOK, loaded));
  EXPECT_TRUE(loaded.empty());
}

TEST_F(BookmarkFileTest, LegacyBackupVerifiesBytesAndRecoversLostRenameWithoutChangingSource) {
  using namespace companion;
  constexpr const char* source = "/.crosspoint/bookmarks/legacy.json";
  constexpr const char* backup = "/.crosspoint/companion/bookmark-legacy-original";
  constexpr const char* temporary = "/.crosspoint/companion/bookmark-legacy-original-next";
  auto& state = inventory_hal_test::state;
  state.files[source] = {'o', 'l', 'd'};
  const auto original = state.files.at(source);
  std::array<uint8_t, 128> scratch{};
  Digest hash{};
  uint64_t length = 0;
  auto file = Storage.open(source, O_RDONLY);
  ASSERT_TRUE(hashInventoryFile(file, scratch, length, hash));
  ASSERT_TRUE(file.close());
  state.failRenameAfterSource = temporary;
  auto owner = makeUniqueNoThrow<HalBookmarkLegacyBackup>(scratch);
  EXPECT_FALSE(owner->create(source, backup, temporary, length, hash));
  owner.reset();
  EXPECT_EQ(state.files.at(source), original);
  EXPECT_EQ(state.files.at(backup), original);
  state.failRenameAfterSource.clear();
  owner = makeUniqueNoThrow<HalBookmarkLegacyBackup>(scratch);
  EXPECT_TRUE(owner->create(source, backup, temporary, length, hash));
  owner.reset();
  state.files[backup] = {9};
  const auto foreign = state.files;
  owner = makeUniqueNoThrow<HalBookmarkLegacyBackup>(scratch);
  EXPECT_FALSE(owner->create(source, backup, temporary, length, hash));
  EXPECT_EQ(state.files, foreign);
}

TEST_F(BookmarkFileTest, LegacyBackupRejectsFailedDurabilityAndForeignTemporaryBytes) {
  using namespace companion;
  constexpr const char* source = "/.crosspoint/bookmarks/legacy.json";
  constexpr const char* backup = "/.crosspoint/companion/bookmark-legacy-original";
  constexpr const char* temporary = "/.crosspoint/companion/bookmark-legacy-original-next";
  for (unsigned mode = 0; mode < 4; ++mode) {
    auto& state = inventory_hal_test::state;
    state = {};
    state.files[source] = {'o', 'l', 'd'};
    const auto original = state.files.at(source);
    std::array<uint8_t, 128> scratch{};
    Digest hash{};
    uint64_t length = 0;
    auto file = Storage.open(source, O_RDONLY);
    ASSERT_TRUE(hashInventoryFile(file, scratch, length, hash));
    ASSERT_TRUE(file.close());
    if (mode == 0) state.failSyncPath = temporary;
    if (mode == 1) state.corruptWritePath = temporary;
    if (mode == 2) state.failClosePath = source;
    if (mode == 3) state.files[temporary] = {9};
    auto owner = makeUniqueNoThrow<HalBookmarkLegacyBackup>(scratch);
    EXPECT_FALSE(owner->create(source, backup, temporary, length, hash));
    owner.reset();
    EXPECT_EQ(state.files.at(source), original);
    EXPECT_FALSE(state.files.contains(backup));
    if (mode == 3) {
      EXPECT_EQ(state.files.at(temporary), (std::vector<uint8_t>{9}));
    }
  }
}

TEST_F(BookmarkFileTest, MigrationClaimRoundtripCorruptionAndImmutableDurability) {
  using namespace companion;
  BookmarkMigrationClaim claim;
  claim.transaction.fill(1);
  claim.storageGeneration.fill(2);
  claim.edition.fill(3);
  claim.originalHash.fill(4);
  claim.sourcePathHash.fill(5);
  claim.originalLength = 123;
  std::array<uint8_t, BOOKMARK_MIGRATION_CLAIM_SIZE> bytes{};
  ASSERT_TRUE(encodeBookmarkMigrationClaim(claim, bytes));
  BookmarkMigrationClaim output;
  ASSERT_TRUE(decodeBookmarkMigrationClaim(bytes, output));
  EXPECT_EQ(output, claim);
  for (size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] ^= 1;
    output = claim;
    EXPECT_FALSE(decodeBookmarkMigrationClaim(bytes, output));
    EXPECT_EQ(output, claim);
    bytes[i] ^= 1;
  }
  constexpr const char* path = "/.crosspoint/companion/bookmark-migration";
  constexpr const char* temporary = "/.crosspoint/companion/bookmark-migration-next";
  ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
  auto owner = makeUniqueNoThrow<HalBookmarkMigrationRecord>();
  auto& state = inventory_hal_test::state;
  state.failSyncPath = temporary;
  EXPECT_FALSE(owner->persist(path, temporary, claim));
  EXPECT_FALSE(owner->persist(path, temporary, claim));
  EXPECT_FALSE(state.files.contains(path));
  state.failSyncPath.clear();
  state.failRenameAfterSource = temporary;
  EXPECT_FALSE(owner->persist(path, temporary, claim));
  state.failRenameAfterSource.clear();
  EXPECT_TRUE(owner->persist(path, temporary, claim));
  EXPECT_EQ(owner->read(path, output), BookmarkPublicationRecord::Matches);
  EXPECT_EQ(output, claim);
  auto foreign = claim;
  foreign.transaction[0] = 9;
  const auto before = state.files;
  EXPECT_FALSE(owner->persist(path, temporary, foreign));
  EXPECT_EQ(state.files, before);
  state.failClosePath = path;
  output = foreign;
  EXPECT_EQ(owner->read(path, output), BookmarkPublicationRecord::Error);
  EXPECT_EQ(output, foreign);
}

TEST_F(BookmarkFileTest, MigrationBackupClaimsOwnershipBeforeCopyAndReclaimsInterruptedPartial) {
  using namespace companion;
  auto& state = inventory_hal_test::state;
  constexpr const char* source = "/.crosspoint/bookmarks/legacy.json";
  state.files[source] = {'o', 'l', 'd'};
  std::array<uint8_t, 128> scratch{};
  Digest edition{};
  Identity transaction{}, generation{};
  edition.fill(7);
  transaction.fill(1);
  generation.fill(2);
  const auto validator = +[](void*, const BookmarkMigrationClaim&) { return true; };
  auto session = makeUniqueNoThrow<HalBookmarkMigrationBackupSession>(scratch, validator, nullptr);
  ASSERT_EQ(session->prepare(source, edition, transaction, generation), TintaJournalResult::Ok);
  const auto claim = session->migrationClaim();
  const std::string backup = session->backupPath();
  session.reset();
  HalBookmarkMigrationPaths paths;
  ASSERT_TRUE(paths.initialize(edition, claim.sourcePathHash));
  ASSERT_TRUE(paths.bind(claim));
  EXPECT_EQ(state.files.at(backup), state.files.at(source));
  state.files.erase(backup);
  state.files[paths.backupTemporary()] = {9};
  transaction.fill(9);
  session = makeUniqueNoThrow<HalBookmarkMigrationBackupSession>(scratch, validator, nullptr);
  ASSERT_EQ(session->prepare(source, edition, transaction, generation), TintaJournalResult::Ok);
  EXPECT_EQ(session->migrationClaim(), claim);
  EXPECT_EQ(state.files.at(backup), state.files.at(source));
  EXPECT_FALSE(state.files.contains(paths.backupTemporary()));
  session.reset();
  const auto before = state.files;
  generation.fill(9);
  session = makeUniqueNoThrow<HalBookmarkMigrationBackupSession>(scratch, validator, nullptr);
  EXPECT_EQ(session->prepare(source, edition, transaction, generation), TintaJournalResult::Invalid);
  EXPECT_EQ(state.files, before);
}

TEST_F(BookmarkFileTest, MigrationClaimPrecedesCopyAndSeparateLocalCopiesRetainSeparateOriginals) {
  using namespace companion;
  auto& state = inventory_hal_test::state;
  constexpr const char* first = "/.crosspoint/bookmarks/first.json";
  constexpr const char* second = "/.crosspoint/bookmarks/second.json";
  state.files[first] = {'a'};
  state.files[second] = {'b'};
  std::array<uint8_t, 128> scratch{};
  Digest edition{};
  Identity transaction{}, generation{};
  edition.fill(7);
  transaction.fill(1);
  generation.fill(2);
  const auto validator = +[](void*, const BookmarkMigrationClaim&) { return true; };
  state.failRenameAfter = 1;
  auto session = makeUniqueNoThrow<HalBookmarkMigrationBackupSession>(scratch, validator, nullptr);
  EXPECT_EQ(session->prepare(first, edition, transaction, generation), TintaJournalResult::IoError);
  const auto claim = session->migrationClaim();
  session.reset();
  HalBookmarkMigrationPaths paths;
  ASSERT_TRUE(paths.initialize(edition, claim.sourcePathHash));
  ASSERT_TRUE(paths.bind(claim));
  EXPECT_TRUE(state.files.contains(paths.record()));
  EXPECT_FALSE(state.files.contains(paths.backup()));
  EXPECT_FALSE(state.files.contains(paths.backupTemporary()));
  state.failRenameAfter = 0;
  session = makeUniqueNoThrow<HalBookmarkMigrationBackupSession>(scratch, validator, nullptr);
  ASSERT_EQ(session->prepare(first, edition, transaction, generation), TintaJournalResult::Ok);
  session.reset();
  transaction.fill(2);
  session = makeUniqueNoThrow<HalBookmarkMigrationBackupSession>(scratch, validator, nullptr);
  ASSERT_EQ(session->prepare(second, edition, transaction, generation), TintaJournalResult::Ok);
  EXPECT_NE(session->migrationClaim().sourcePathHash, claim.sourcePathHash);
  EXPECT_EQ(state.files.at(paths.backup()), (std::vector<uint8_t>{'a'}));
  EXPECT_EQ(state.files.at(session->backupPath()), (std::vector<uint8_t>{'b'}));
}
TEST_F(BookmarkFileTest, MigrationCannotClaimPreexistingForeignBackupNamespace) {
  using namespace companion;
  auto& state = inventory_hal_test::state;
  constexpr const char* source = "/.crosspoint/bookmarks/legacy.json";
  state.files[source] = {'a'};
  std::array<uint8_t, 128> scratch{};
  Digest edition{};
  Identity transaction{}, generation{};
  edition.fill(7);
  transaction.fill(1);
  generation.fill(2);
  const auto validator = +[](void*, const BookmarkMigrationClaim&) { return true; };
  auto session = makeUniqueNoThrow<HalBookmarkMigrationBackupSession>(scratch, validator, nullptr);
  ASSERT_EQ(session->prepare(source, edition, transaction, generation), TintaJournalResult::Ok);
  const auto claim = session->migrationClaim();
  session.reset();
  HalBookmarkMigrationPaths paths;
  ASSERT_TRUE(paths.initialize(edition, claim.sourcePathHash));
  ASSERT_TRUE(paths.bind(claim));
  state.files.erase(paths.record());
  state.files.erase(paths.backup());
  state.files[paths.backupTemporary()] = {9};
  const auto before = state.files;
  session = makeUniqueNoThrow<HalBookmarkMigrationBackupSession>(scratch, validator, nullptr);
  EXPECT_EQ(session->prepare(source, edition, transaction, generation), TintaJournalResult::Corrupt);
  EXPECT_EQ(state.files, before);
  EXPECT_FALSE(state.files.contains(paths.record()));
}

TEST_F(BookmarkFileTest, ExplicitBackupLoadPreservesLegacyFieldsAndRejectsFailedReadWithoutTouchingCanonical) {
  constexpr const char* backup = "/.crosspoint/companion/bookmark-original-test";
  install(R"({"bookmarks":[{"name":"Current","vo":1}]})");
  const std::string longName(129, 'x');
  const std::string json =
      "{\"bookmarks\":[{\"name\":\"" + longName +
      "\",\"summary\":\"Old\",\"xpath\":\"/body/p[1]\",\"percentage\":0.5,\"si\":2,\"pc\":10,\"pp\":5,\"vo\":42}]}";
  inventory_hal_test::state.files[backup] = std::vector<uint8_t>(json.begin(), json.end());
  const auto before = inventory_hal_test::state.files;
  std::vector<BookmarkEntry> entries;
  ASSERT_TRUE(BookmarkFile::loadFromPath(backup, entries));
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].name, longName);
  EXPECT_EQ(entries[0].summary, "Old");
  EXPECT_EQ(entries[0].xpath, "/body/p[1]");
  EXPECT_EQ(entries[0].computedSpineIndex, 2);
  EXPECT_EQ(entries[0].visibleTextOffset, 42u);
  EXPECT_TRUE(entries[0].hasVisibleTextOffset);
  EXPECT_EQ(entries[0].computedChapterPageCount, 10);
  EXPECT_EQ(entries[0].computedChapterProgress, 5);
  EXPECT_FLOAT_EQ(entries[0].percentage, 0.5);
  EXPECT_EQ(inventory_hal_test::state.files, before);
  bookmark_test::failRead = true;
  EXPECT_FALSE(BookmarkFile::loadFromPath(backup, entries));
  EXPECT_TRUE(entries.empty());
  EXPECT_FALSE(BookmarkFile::loadFromPath(nullptr, entries));
  EXPECT_EQ(inventory_hal_test::state.files, before);
}

TEST_F(BookmarkFileTest, StrictBackupLoadRejectsCoercedOrTruncatedLegacyFields) {
  constexpr const char* backup = "/.crosspoint/companion/bookmark-original-test";
  for (const auto fields :
       {R"("name":4)", R"("summary":"ok\u0000hidden")", R"("si":65536)", R"("si":-1)", R"("si":"2")", R"("si":1.5)",
        R"("vo":4294967296,"si":2)", R"("vo":42)", R"("percentage":"bad")"}) {
    const std::string json = std::string("{\"bookmarks\":[{") + fields + "}]}";
    inventory_hal_test::state.files[backup] = std::vector<uint8_t>(json.begin(), json.end());
    const auto before = inventory_hal_test::state.files;
    std::vector<BookmarkEntry> entries;
    EXPECT_FALSE(BookmarkFile::loadFromPath(backup, entries));
    EXPECT_TRUE(entries.empty());
    EXPECT_EQ(inventory_hal_test::state.files, before);
  }
}

namespace {
class MigrationIdentities final : public companion::IdentityStorage {
 public:
  bool hardwareIdentity(companion::Identity& out) override {
    out.fill(9);
    return true;
  }
  bool cardIdentity(companion::Identity& out) override {
    out.fill(2);
    return true;
  }
  companion::IdentityRead readBinding(std::span<uint8_t>) override { return companion::IdentityRead::Missing; }
  bool writeBinding(std::span<const uint8_t>) override { return true; }
  companion::IdentityRead readMarker(companion::Identity&) override { return companion::IdentityRead::Missing; }
  bool createMarker(const companion::Identity&) override { return true; }
  bool randomIdentity(companion::Identity& out) override {
    out.fill(random++);
    return true;
  }
  uint8_t random = 8;
};
}  // namespace
TEST_F(BookmarkFileTest, CanonicalRestoreRequiresExplicitLegacyDecisionAndRetainsOriginal) {
  using namespace companion;
  for (const auto decision : {LegacyBookmarkDecision::Associate, LegacyBookmarkDecision::LeaveUnassociated}) {
    auto& state = inventory_hal_test::state;
    state = {};
    state.enumerateFileMap = true;
    ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
    install(R"({"bookmarks":[{"name":"Legacy","si":2,"vo":42}]})");
    const auto legacy = BookmarkUtil::getBookmarkPath(BOOK);
    const auto original = state.files.at(legacy);
    Digest edition{};
    edition.fill(7);
    Identity generation{};
    generation.fill(2);
    std::array<char, 128> canonical{};
    ASSERT_TRUE(BookmarkUtil::getBookmarkEditionPath(edition, canonical));
    MigrationIdentities identities;
    std::array<uint8_t, 128> scratch{};
    const auto makeSession = [&] {
      return makeUniqueNoThrow<NativeBookmarkReaderSession>(
          scratch, edition, generation, identities, +[](void*) { return true; },
          +[](void*, const BookmarkMigrationClaim&) { return true; },
          +[](void*, const BookmarkPublicationClaim&) { return true; },
          +[](void*, const BookmarkPreparationClaim&) { return true; }, nullptr);
    };
    std::vector<BookmarkEntry> entries(1);
    entries[0].name = "Unverified";
    auto session = makeSession();
    ASSERT_TRUE(session);
    EXPECT_EQ(session->restoreEdition(canonical.data(), legacy.c_str(), 4, entries), TintaJournalResult::Conflict);
    EXPECT_TRUE(session->needsLegacyAssociation());
    EXPECT_FALSE(session->requiresRecovery());
    EXPECT_FALSE(tinta_body_detail::nonzero(session->conflictIdentity()));
    EXPECT_TRUE(entries.empty());
    EXPECT_EQ(identities.random, 8);
    EXPECT_EQ(state.files.at(legacy), original);
    EXPECT_FALSE(state.files.contains(canonical.data()));
    session = makeSession();
    ASSERT_TRUE(session);
    ASSERT_EQ(session->restoreEdition(canonical.data(), legacy.c_str(), 4, entries, decision), TintaJournalResult::Ok);
    EXPECT_FALSE(session->needsLegacyAssociation());
    EXPECT_FALSE(session->requiresRecovery());
    EXPECT_EQ(state.files.at(legacy), original);
    ASSERT_EQ(entries.size(), decision == LegacyBookmarkDecision::Associate ? 1u : 0u);
    if (!entries.empty()) {
      EXPECT_EQ(entries[0].name, "Legacy");
    }
    const auto files = state.files;
    session = makeSession();
    ASSERT_TRUE(session);
    ASSERT_EQ(session->restoreEdition(canonical.data(), legacy.c_str(), 4, entries), TintaJournalResult::Ok);
    EXPECT_EQ(state.files, files);
    EXPECT_EQ(state.files.at(legacy), original);
    EXPECT_FALSE(session->needsLegacyAssociation());
  }
}
TEST_F(BookmarkFileTest, CanonicalRestoreRecoversEitherPublicationLayoutBeforeLegacyDecision) {
  using namespace companion;
  for (const bool legacyPending : {false, true}) {
    auto& state = inventory_hal_test::state;
    state = {};
    state.enumerateFileMap = true;
    ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
    install(R"({"bookmarks":[{"name":"Legacy","si":2,"vo":42}]})");
    const auto legacy = BookmarkUtil::getBookmarkPath(BOOK);
    const auto original = state.files.at(legacy);
    Digest edition{};
    edition.fill(7);
    Identity generation{};
    generation.fill(2);
    std::array<char, 128> canonical{};
    ASSERT_TRUE(BookmarkUtil::getBookmarkEditionPath(edition, canonical));
    MigrationIdentities identities;
    std::array<uint8_t, 128> scratch{};
    const auto makeSession = [&] {
      return makeUniqueNoThrow<NativeBookmarkReaderSession>(
          scratch, edition, generation, identities, +[](void*) { return true; },
          +[](void*, const BookmarkMigrationClaim&) { return true; },
          +[](void*, const BookmarkPublicationClaim&) { return true; },
          +[](void*, const BookmarkPreparationClaim&) { return true; }, nullptr);
    };
    std::vector<BookmarkEntry> entries;
    auto session = makeSession();
    ASSERT_TRUE(session);
    if (legacyPending) {
      ASSERT_EQ(session->restore(legacy.c_str(), 4, entries), TintaJournalResult::Ok);
      const auto selected = entries[0];
      session = makeSession();
      state.failRenameAfterSource = HalBookmarkPublicationPaths::INTENT_NEXT;
      EXPECT_EQ(session->rename(legacy.c_str(), 4, selected, "Renamed", entries), TintaJournalResult::IoError);
    } else {
      state.failRenameAfterSource = HalBookmarkPublicationPaths::INTENT_NEXT;
      EXPECT_EQ(
          session->restoreEdition(canonical.data(), legacy.c_str(), 4, entries, LegacyBookmarkDecision::Associate),
          TintaJournalResult::IoError);
      EXPECT_TRUE(entries.empty());
    }
    EXPECT_TRUE(session->requiresRecovery());
    session.reset();
    state.failRenameAfterSource.clear();
    session = makeSession();
    ASSERT_TRUE(session);
    const auto restored = session->restoreEdition(canonical.data(), legacy.c_str(), 4, entries);
    if (legacyPending) {
      ASSERT_EQ(restored, TintaJournalResult::Conflict);
      EXPECT_TRUE(session->needsLegacyAssociation());
      EXPECT_TRUE(entries.empty());
      session = makeSession();
      ASSERT_EQ(session->restoreEdition(canonical.data(), legacy.c_str(), 4, entries,
                                        LegacyBookmarkDecision::LeaveUnassociated),
                TintaJournalResult::Ok);
    } else {
      ASSERT_EQ(restored, TintaJournalResult::Ok);
      EXPECT_EQ(state.files.at(legacy), original);
    }
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].name, legacyPending ? "Renamed" : "Legacy");
    EXPECT_FALSE(session->needsLegacyAssociation());
    EXPECT_FALSE(session->requiresRecovery());
    EXPECT_FALSE(state.files.contains(HalBookmarkPublicationPaths::INTENT));
    EXPECT_FALSE(state.files.contains(HalBookmarkPreparationStorage::PATH));
    const auto files = state.files;
    session = makeSession();
    ASSERT_EQ(session->restoreEdition(canonical.data(), legacy.c_str(), 4, entries), TintaJournalResult::Ok);
    EXPECT_EQ(state.files, files);
  }
}
TEST_F(BookmarkFileTest, ExplicitLegacyAssociationRebindsForeignIdsAndPreservesCurrentPutsAndDeletes) {
  using namespace companion;
  for (unsigned mode = 0; mode < 3; ++mode) {
    auto& state = inventory_hal_test::state;
    state = {};
    state.enumerateFileMap = true;
    ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
    install(R"({"bookmarks":[{"id":"03000000000000000000000000000000","name":"Legacy","si":2,"vo":42}]})");
    const auto legacy = BookmarkUtil::getBookmarkPath(BOOK);
    const auto original = state.files.at(legacy);
    Digest edition{}, previousEdition{};
    edition.fill(7);
    previousEdition.fill(mode == 0 ? 9 : 7);
    Identity generation{};
    generation.fill(2);
    MigrationIdentities identities;
    BookmarkBodyView bookmark;
    bookmark.identity[0] = 3;
    bookmark.anchor = {1, 2};
    static constexpr uint8_t NAME[] = {'N', 'a', 't', 'i', 'v', 'e'};
    bookmark.name = NAME;
    auto save = makeUniqueNoThrow<NativeBookmarkSaveSession>();
    ASSERT_TRUE(save);
    ASSERT_EQ(save->persist(bookmark, previousEdition, identities), TintaJournalResult::Ok);
    save.reset();
    if (mode == 2) {
      bookmark.deleted = true;
      bookmark.name = {};
      save = makeUniqueNoThrow<NativeBookmarkSaveSession>();
      ASSERT_TRUE(save);
      ASSERT_EQ(save->persist(bookmark, previousEdition, identities), TintaJournalResult::Ok);
      save.reset();
    }
    std::array<char, 128> canonical{};
    ASSERT_TRUE(BookmarkUtil::getBookmarkEditionPath(edition, canonical));
    std::array<uint8_t, 128> scratch{};
    auto session = makeUniqueNoThrow<NativeBookmarkReaderSession>(
        scratch, edition, generation, identities, +[](void*) { return true; },
        +[](void*, const BookmarkMigrationClaim&) { return true; },
        +[](void*, const BookmarkPublicationClaim&) { return true; },
        +[](void*, const BookmarkPreparationClaim&) { return true; }, nullptr);
    ASSERT_TRUE(session);
    std::vector<BookmarkEntry> entries;
    ASSERT_EQ(session->restoreEdition(canonical.data(), legacy.c_str(), 4, entries, LegacyBookmarkDecision::Associate),
              TintaJournalResult::Ok);
    EXPECT_EQ(state.files.at(legacy), original);
    ASSERT_EQ(entries.size(), mode == 2 ? 0u : 1u);
    if (mode == 0) {
      EXPECT_NE(entries[0].identity, bookmark.identity);
      EXPECT_TRUE(BookmarkIdentity::valid(entries[0].identity));
      EXPECT_EQ(entries[0].name, "Legacy");
    } else if (mode == 1) {
      EXPECT_EQ(entries[0].identity, bookmark.identity);
      EXPECT_EQ(entries[0].name, "Native");
    }
    auto audit = makeUniqueNoThrow<HalJournalCausalAuditSession>();
    ASSERT_TRUE(audit);
    ASSERT_TRUE(audit->run());
    EXPECT_EQ(audit->recordCount(), mode == 1 ? 1u : 2u);
    std::array<uint8_t, MAX_BOOKMARK_BODY_SIZE> body{};
    size_t length = 0;
    ASSERT_EQ(audit->resolveBookmark(previousEdition, bookmark.identity, body, length), TintaJournalResult::Ok);
    BookmarkBodyView resolved;
    ASSERT_TRUE(decodeBookmarkBody(std::span(body).first(length), resolved));
    EXPECT_EQ(resolved.deleted, mode == 2);
    if (mode != 2) {
      EXPECT_EQ(std::string(resolved.name.begin(), resolved.name.end()), "Native");
    }
  }
}
TEST_F(BookmarkFileTest, CanonicalEmptyRestoreDoesNotImportLegacyFilesAddedLater) {
  using namespace companion;
  auto& state = inventory_hal_test::state;
  state.enumerateFileMap = true;
  ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
  const auto legacy = BookmarkUtil::getBookmarkPath(BOOK);
  Digest edition{};
  edition.fill(7);
  Identity generation{};
  generation.fill(2);
  std::array<char, 128> canonical{};
  ASSERT_TRUE(BookmarkUtil::getBookmarkEditionPath(edition, canonical));
  MigrationIdentities identities;
  std::array<uint8_t, 128> scratch{};
  const auto makeSession = [&] {
    return makeUniqueNoThrow<NativeBookmarkReaderSession>(
        scratch, edition, generation, identities, +[](void*) { return true; },
        +[](void*, const BookmarkMigrationClaim&) { return true; },
        +[](void*, const BookmarkPublicationClaim&) { return true; },
        +[](void*, const BookmarkPreparationClaim&) { return true; }, nullptr);
  };
  std::vector<BookmarkEntry> entries(1);
  entries[0].name = "Unverified";
  auto session = makeSession();
  ASSERT_TRUE(session);
  ASSERT_EQ(session->restoreEdition(canonical.data(), legacy.c_str(), 4, entries), TintaJournalResult::Ok);
  EXPECT_TRUE(entries.empty());
  EXPECT_FALSE(session->needsLegacyAssociation());
  const auto& bytes = state.files.at(canonical.data());
  EXPECT_EQ(std::string(bytes.begin(), bytes.end()), R"({"bookmarks":[]})");
  const auto checkedFiles = state.files;
  state.readErrorPath = canonical.data();
  entries.emplace_back();
  entries.back().name = "Unverified retry row";
  session = makeSession();
  ASSERT_TRUE(session);
  EXPECT_EQ(session->restoreEdition(canonical.data(), legacy.c_str(), 4, entries), TintaJournalResult::IoError);
  EXPECT_TRUE(entries.empty());
  EXPECT_EQ(state.files, checkedFiles);
  state.readErrorPath.clear();
  session = makeSession();
  ASSERT_TRUE(session);
  ASSERT_EQ(session->restoreEdition(canonical.data(), legacy.c_str(), 4, entries), TintaJournalResult::Ok);
  EXPECT_TRUE(entries.empty());
  EXPECT_EQ(state.files, checkedFiles);
  install(R"({"bookmarks":[{"name":"Added later","si":2,"vo":42}]})");
  const auto files = state.files;
  session = makeSession();
  ASSERT_TRUE(session);
  ASSERT_EQ(session->restoreEdition(canonical.data(), legacy.c_str(), 4, entries), TintaJournalResult::Ok);
  EXPECT_TRUE(entries.empty());
  EXPECT_EQ(state.files, files);
  auto audit = makeUniqueNoThrow<HalJournalCausalAuditSession>();
  ASSERT_TRUE(audit);
  ASSERT_TRUE(audit->run());
  EXPECT_EQ(audit->recordCount(), 0u);
}
TEST_F(BookmarkFileTest, NativeMigrationReadsRetainedBackupAndPublishesJournalDerivedStableIdentities) {
  using namespace companion;
  ASSERT_TRUE(Storage.ensureDirectoryExists(HalBookmarkPublicationPaths::ACTIVE_PARENT));
  inventory_hal_test::state.enumerateFileMap = true;
  install(R"({"bookmarks":[{"name":"Old","summary":"Legacy","si":2,"vo":42}]})");
  const auto source = BookmarkUtil::getBookmarkPath(BOOK);
  const auto original = inventory_hal_test::state.files.at(source);
  std::array<uint8_t, 128> scratch{};
  Digest edition{};
  Identity migrationId{}, publicationId{}, generation{};
  edition.fill(7);
  migrationId.fill(1);
  publicationId.fill(3);
  generation.fill(2);
  const auto migrationValidator = +[](void*, const BookmarkMigrationClaim&) { return true; };
  const auto publicationValidator = +[](void*, const BookmarkPublicationClaim&) { return true; };
  MigrationIdentities identities;
  std::vector<BookmarkEntry> entries;
  auto migration = makeUniqueNoThrow<NativeBookmarkMigrationSession>(scratch, migrationValidator, nullptr);
  ASSERT_EQ(migration->importLegacy(source.c_str(), edition, migrationId, generation, 4, identities, entries),
            TintaJournalResult::Ok);
  const auto claim = migration->migrationClaim();
  migration.reset();
  EXPECT_EQ(inventory_hal_test::state.files.at(source), original);
  HalBookmarkMigrationPaths paths;
  ASSERT_TRUE(paths.initialize(edition, claim.sourcePathHash));
  ASSERT_TRUE(paths.bind(claim));
  EXPECT_EQ(inventory_hal_test::state.files.at(paths.backup()), original);
  auto publisher = makeUniqueNoThrow<NativeBookmarkPublicationSession>(
      scratch, publicationValidator, nullptr, +[](void*, const BookmarkPreparationClaim&) { return true; });
  ASSERT_EQ(publisher->publish(source, edition, 4, publicationId, generation), TintaJournalResult::Ok);
  publisher.reset();
  ASSERT_TRUE(BookmarkFile::load(BOOK, entries));
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_TRUE(BookmarkIdentity::valid(entries[0].identity));
  EXPECT_EQ(entries[0].name, "Old");
  EXPECT_EQ(entries[0].visibleTextOffset, 42u);
  EXPECT_EQ(inventory_hal_test::state.files.at(paths.backup()), original);
  const auto journal = inventory_hal_test::state.files.at(TINTA_JOURNAL_EVENTS);
  migration = makeUniqueNoThrow<NativeBookmarkMigrationSession>(scratch, migrationValidator, nullptr);
  ASSERT_EQ(migration->importLegacy(source.c_str(), edition, migrationId, generation, 4, identities, entries),
            TintaJournalResult::Ok);
  EXPECT_EQ(inventory_hal_test::state.files.at(TINTA_JOURNAL_EVENTS), journal);
}

TEST_F(BookmarkFileTest, MigrationRequiresResolvedLegacyAnchorsAndRechecksContextBeforeImport) {
  using namespace companion;
  for (unsigned mode = 0; mode < 3; ++mode) {
    auto& state = inventory_hal_test::state;
    state = {};
    state.enumerateFileMap = true;
    bookmark_test::failRead = false;
    install(R"({"bookmarks":[{"name":"Old","si":2,"pc":10,"pp":5}]})");
    const auto source = BookmarkUtil::getBookmarkPath(BOOK);
    const auto original = state.files.at(source);
    std::array<uint8_t, 128> scratch{};
    Digest edition{};
    Identity transaction{}, generation{};
    edition.fill(7);
    transaction.fill(1);
    generation.fill(2);
    struct Context {
      bool valid = true;
      bool change = false;
    } context;
    context.change = mode == 2;
    const auto validator =
        +[](void* opaque, const BookmarkMigrationClaim&) { return static_cast<Context*>(opaque)->valid; };
    const auto resolver = +[](void* opaque, BookmarkEntry& entry) {
      entry.visibleTextOffset = 73;
      entry.hasVisibleTextOffset = true;
      auto& ctx = *static_cast<Context*>(opaque);
      if (ctx.change) ctx.valid = false;
      return true;
    };
    MigrationIdentities identities;
    std::vector<BookmarkEntry> entries;
    auto session = makeUniqueNoThrow<NativeBookmarkMigrationSession>(scratch, validator, &context);
    EXPECT_EQ(session->importLegacy(source.c_str(), edition, transaction, generation, 4, identities, entries,
                                    mode == 0 ? nullptr : resolver),
              mode == 1 ? TintaJournalResult::Ok : TintaJournalResult::Invalid);
    EXPECT_EQ(state.files.at(source), original);
    EXPECT_EQ(state.files.contains(TINTA_JOURNAL_EVENTS), mode == 1);
  }
}

TEST_F(BookmarkFileTest, PreparationClaimRequiresDurableReadbackAndPreservesForeignRecords) {
  using namespace companion;
  BookmarkPreparationClaim claim;
  claim.transaction.fill(1);
  claim.storageGeneration.fill(2);
  claim.edition.fill(3);
  claim.destinationPathHash.fill(4);
  constexpr const char* path = "/.crosspoint/companion/bookmark-preparation";
  constexpr const char* temporary = "/.crosspoint/companion/bookmark-preparation-next";
  ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
  auto owner = makeUniqueNoThrow<HalBookmarkPreparationRecord>();
  ASSERT_TRUE(owner);
  auto& state = inventory_hal_test::state;
  state.failSyncPath = temporary;
  EXPECT_FALSE(owner->persist(path, temporary, claim));
  EXPECT_FALSE(owner->persist(path, temporary, claim));
  EXPECT_FALSE(state.files.contains(path));
  state.failSyncPath.clear();
  state.failRenameAfterSource = temporary;
  EXPECT_FALSE(owner->persist(path, temporary, claim));
  state.failRenameAfterSource.clear();
  ASSERT_TRUE(owner->persist(path, temporary, claim));
  BookmarkPreparationClaim output;
  ASSERT_EQ(owner->read(path, output), BookmarkPublicationRecord::Matches);
  EXPECT_EQ(output, claim);
  auto foreign = claim;
  foreign.storageGeneration[0] = 9;
  const auto before = state.files;
  EXPECT_FALSE(owner->persist(path, temporary, foreign));
  EXPECT_EQ(state.files, before);
  state.failClosePath = path;
  output = foreign;
  EXPECT_EQ(owner->read(path, output), BookmarkPublicationRecord::Error);
  EXPECT_EQ(output, foreign);
  state.failClosePath.clear();
  state.files[path][0] ^= 1;
  const auto corrupt = state.files;
  EXPECT_EQ(owner->read(path, output), BookmarkPublicationRecord::Other);
  EXPECT_EQ(output, foreign);
  EXPECT_FALSE(owner->persist(path, temporary, claim));
  EXPECT_EQ(state.files, corrupt);
}

namespace {
companion::BookmarkPreparationClaim halPreparationClaim() {
  companion::BookmarkPreparationClaim claim;
  claim.transaction.fill(1);
  claim.storageGeneration.fill(2);
  claim.edition.fill(3);
  claim.destinationPathHash.fill(4);
  return claim;
}
}  // namespace
TEST_F(BookmarkFileTest, PreparationHalRecoversOwnedOrphansAndLostRemovalAcknowledgement) {
  using namespace companion;
  ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
  const auto claim = halPreparationClaim();
  auto storage = makeUniqueNoThrow<HalBookmarkPreparationStorage>(
      +[](void*, const BookmarkPreparationClaim&) { return true; }, nullptr);
  ASSERT_TRUE(storage);
  BookmarkPreparation owner(*storage);
  ASSERT_EQ(owner.begin(claim), TintaJournalResult::Ok);
  auto& state = inventory_hal_test::state;
  state.files[HalBookmarkIdentityStage::PATH] = {1};
  state.files[HalBookmarkBodyStage::PATH] = {2};
  state.files[HalBookmarkJsonStage::PATH] = {3};
  state.failRemoveAfter = true;
  EXPECT_EQ(owner.recover(claim), TintaJournalResult::IoError);
  EXPECT_TRUE(state.files.contains(HalBookmarkPreparationStorage::PATH));
  state.failRemoveAfter = false;
  storage.reset();
  storage = makeUniqueNoThrow<HalBookmarkPreparationStorage>(
      +[](void*, const BookmarkPreparationClaim&) { return true; }, nullptr);
  BookmarkPreparationClaim loaded;
  ASSERT_EQ(storage->load(loaded), BookmarkPublicationRecord::Matches);
  EXPECT_EQ(loaded, claim);
  BookmarkPreparation retry(*storage);
  EXPECT_EQ(retry.recover(loaded), TintaJournalResult::Ok);
  EXPECT_FALSE(state.files.contains(HalBookmarkIdentityStage::PATH));
  EXPECT_FALSE(state.files.contains(HalBookmarkBodyStage::PATH));
  EXPECT_FALSE(state.files.contains(HalBookmarkJsonStage::PATH));
  EXPECT_FALSE(state.files.contains(HalBookmarkPreparationStorage::PATH));
}
TEST_F(BookmarkFileTest, PreparationHalProtectsTemporaryPublicationIntentAndDirectories) {
  using namespace companion;
  ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
  const auto claim = halPreparationClaim();
  auto storage = makeUniqueNoThrow<HalBookmarkPreparationStorage>(
      +[](void*, const BookmarkPreparationClaim&) { return true; }, nullptr);
  BookmarkPreparation owner(*storage);
  ASSERT_EQ(owner.begin(claim), TintaJournalResult::Ok);
  auto& state = inventory_hal_test::state;
  state.files[HalBookmarkJsonStage::PATH] = {3};
  state.files[HalBookmarkPublicationPaths::INTENT_NEXT] = {0};
  auto before = state.files;
  EXPECT_EQ(owner.recover(claim), TintaJournalResult::Conflict);
  EXPECT_FALSE(storage->remove(BookmarkPreparationRole::Json));
  EXPECT_EQ(state.files, before);
  state.files.erase(HalBookmarkPublicationPaths::INTENT_NEXT);
  state.directories[HalBookmarkBodyStage::PATH] = {};
  before = state.files;
  EXPECT_EQ(owner.recover(claim), TintaJournalResult::Corrupt);
  EXPECT_FALSE(storage->clear(claim));
  EXPECT_EQ(state.files, before);
}
TEST_F(BookmarkFileTest, PreparationHalPromotesTemporaryAuthorityOnlyInVerifiedContext) {
  using namespace companion;
  ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
  const auto claim = halPreparationClaim();
  std::array<uint8_t, BOOKMARK_PREPARATION_CLAIM_SIZE> bytes{};
  ASSERT_TRUE(encodeBookmarkPreparationClaim(claim, bytes));
  auto& state = inventory_hal_test::state;
  state.files[HalBookmarkPreparationStorage::NEXT] = {bytes.begin(), bytes.end()};
  bool context = false;
  auto storage = makeUniqueNoThrow<HalBookmarkPreparationStorage>(
      +[](void* ctx, const BookmarkPreparationClaim&) { return *static_cast<bool*>(ctx); }, &context);
  BookmarkPreparationClaim output;
  const auto untouched = output;
  const auto before = state.files;
  EXPECT_EQ(storage->load(output), BookmarkPublicationRecord::Other);
  EXPECT_EQ(output, untouched);
  EXPECT_EQ(state.files, before);
  context = true;
  state.failSyncPath = HalBookmarkPreparationStorage::NEXT;
  EXPECT_EQ(storage->load(output), BookmarkPublicationRecord::Error);
  EXPECT_EQ(output, untouched);
  EXPECT_FALSE(state.files.contains(HalBookmarkPreparationStorage::PATH));
  state.failSyncPath.clear();
  ASSERT_EQ(storage->load(output), BookmarkPublicationRecord::Matches);
  EXPECT_EQ(output, claim);
  EXPECT_TRUE(state.files.contains(HalBookmarkPreparationStorage::PATH));
  EXPECT_FALSE(state.files.contains(HalBookmarkPreparationStorage::NEXT));
}

TEST_F(BookmarkFileTest, PreparationSessionBindsRecoveryToDestinationEditionAndCard) {
  using namespace companion;
  ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
  auto& state = inventory_hal_test::state;
  const auto claim = halPreparationClaim();
  const auto validator = +[](void*, const BookmarkPreparationClaim&) { return true; };
  auto session = makeUniqueNoThrow<NativeBookmarkPreparationSession>(validator, nullptr);
  ASSERT_TRUE(session);
  ASSERT_EQ(
      session->begin("/.crosspoint/bookmarks/Book.json", claim.edition, claim.transaction, claim.storageGeneration),
      TintaJournalResult::Ok);
  ASSERT_EQ(session->guard(), TintaJournalResult::Ok);
  state.files[HalBookmarkIdentityStage::PATH] = {1, 2};
  state.files[HalBookmarkJsonStage::PATH] = {3};
  session.reset();
  const auto retained = state.files;
  for (int wrong = 0; wrong < 4; ++wrong) {
    auto edition = claim.edition;
    auto generation = claim.storageGeneration;
    if (wrong == 1) edition[0] ^= 1;
    if (wrong == 2) generation[0] ^= 1;
    auto transaction = claim.transaction;
    if (wrong == 3) transaction[0] ^= 1;
    session = makeUniqueNoThrow<NativeBookmarkPreparationSession>(validator, nullptr);
    EXPECT_EQ(session->recover(wrong == 0 ? "/.crosspoint/bookmarks/Other.json" : "/.crosspoint/bookmarks/book.json",
                               edition, generation, wrong == 3 ? &transaction : nullptr),
              TintaJournalResult::Corrupt);
    EXPECT_EQ(state.files, retained);
  }
  session = makeUniqueNoThrow<NativeBookmarkPreparationSession>(validator, nullptr);
  EXPECT_EQ(session->recover("/.crosspoint/bookmarks/book.json", claim.edition, claim.storageGeneration),
            TintaJournalResult::Ok);
  EXPECT_FALSE(state.files.contains(HalBookmarkIdentityStage::PATH));
  EXPECT_FALSE(state.files.contains(HalBookmarkJsonStage::PATH));
  EXPECT_FALSE(state.files.contains(HalBookmarkPreparationStorage::PATH));
}
TEST_F(BookmarkFileTest, PreparationSessionGuardsContextAndRefusesPublicationHandoffCleanup) {
  using namespace companion;
  ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
  const auto claim = halPreparationClaim();
  bool valid = true;
  const auto validator = +[](void* opaque, const BookmarkPreparationClaim&) { return *static_cast<bool*>(opaque); };
  auto session = makeUniqueNoThrow<NativeBookmarkPreparationSession>(validator, &valid);
  ASSERT_EQ(
      session->begin("/.crosspoint/bookmarks/book.json", claim.edition, claim.transaction, claim.storageGeneration),
      TintaJournalResult::Ok);
  auto& state = inventory_hal_test::state;
  state.files[HalBookmarkJsonStage::PATH] = {3};
  const auto before = state.files;
  valid = false;
  EXPECT_EQ(session->guard(), TintaJournalResult::Invalid);
  EXPECT_EQ(session->discard(), TintaJournalResult::Invalid);
  EXPECT_EQ(state.files, before);
  valid = true;
  state.files[HalBookmarkPublicationPaths::INTENT_NEXT] = {9};
  const auto handedOff = state.files;
  EXPECT_EQ(session->guard(), TintaJournalResult::Conflict);
  EXPECT_EQ(session->discard(), TintaJournalResult::Conflict);
  EXPECT_EQ(state.files, handedOff);
}

TEST_F(BookmarkFileTest, BookmarkStageRetainsOwnedBytesWhenPreparationAuthorityIsLost) {
  using namespace companion;
  ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
  for (const bool sealed : {false, true}) {
    bool authority = true;
    const auto guard = +[](void* ctx) { return *static_cast<bool*>(ctx); };
    std::array<uint8_t, 128> scratch{};
    auto stage = makeUniqueNoThrow<HalBookmarkJsonStage>(scratch, guard, &authority);
    ASSERT_TRUE(stage);
    ASSERT_TRUE(stage->begin(0));
    if (sealed) {
      ASSERT_TRUE(stage->finish());
    }
    authority = false;
    const auto before = inventory_hal_test::state.files;
    EXPECT_FALSE(stage->cleanup());
    EXPECT_EQ(inventory_hal_test::state.files, before);
    stage.reset();
    EXPECT_EQ(inventory_hal_test::state.files, before);
    inventory_hal_test::state.files.erase(HalBookmarkJsonStage::PATH);
  }
}
TEST_F(BookmarkFileTest, TransferCancellationStillCleansUpWhenSeparateAuthorityRemainsValid) {
  using namespace companion;
  ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
  bool progress = true, authority = true;
  const auto flag = +[](void* ctx) { return *static_cast<bool*>(ctx); };
  std::array<uint8_t, 128> scratch{};
  auto stage = makeUniqueNoThrow<HalVerifiedFileStage>(scratch, flag, &progress, TRANSFER_DIRECTORY, flag, &authority);
  constexpr const char* path = "/.crosspoint/companion/cancelled-stage";
  ASSERT_TRUE(stage->begin(path, 10));
  progress = false;
  EXPECT_TRUE(stage->cleanup());
  EXPECT_FALSE(inventory_hal_test::state.files.contains(path));
}

TEST_F(BookmarkFileTest, ReaderLifecycleMigratesEditsAndRepeatedRestorePreservesLocalWork) {
  using namespace companion;
  inventory_hal_test::state.enumerateFileMap = true;
  ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
  install(R"({"bookmarks":[{"name":"Original","summary":"Legacy","si":2,"vo":42}]})");
  const auto path = BookmarkUtil::getBookmarkPath(BOOK);
  Digest edition{};
  edition.fill(7);
  Identity generation{};
  generation.fill(2);
  MigrationIdentities identities;
  std::array<uint8_t, 128> scratch{};
  bool valid = true;
  const auto makeSession = [&] {
    return makeUniqueNoThrow<NativeBookmarkReaderSession>(
        scratch, edition, generation, identities, +[](void* ctx) { return *static_cast<bool*>(ctx); },
        +[](void* ctx, const BookmarkMigrationClaim&) { return *static_cast<bool*>(ctx); },
        +[](void* ctx, const BookmarkPublicationClaim&) { return *static_cast<bool*>(ctx); },
        +[](void* ctx, const BookmarkPreparationClaim&) { return *static_cast<bool*>(ctx); }, &valid);
  };
  std::vector<BookmarkEntry> entries;
  auto session = makeSession();
  ASSERT_EQ(session->restore(path.c_str(), 4, entries), TintaJournalResult::Ok);
  ASSERT_EQ(entries.size(), 1u);
  ASSERT_TRUE(BookmarkIdentity::valid(entries[0].identity));
  const auto original = entries[0];
  session = makeSession();
  ASSERT_EQ(session->rename(path.c_str(), 4, original, "Renamed", entries), TintaJournalResult::Ok);
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].identity, original.identity);
  EXPECT_EQ(entries[0].name, "Renamed");
  BookmarkEntry added{};
  added.name = "New";
  added.summary = "Second";
  added.hasVisibleTextOffset = true;
  added.computedSpineIndex = 1;
  added.visibleTextOffset = 12;
  session = makeSession();
  ASSERT_EQ(session->create(path.c_str(), 4, added, entries), TintaJournalResult::Ok);
  ASSERT_TRUE(BookmarkIdentity::valid(added.identity));
  ASSERT_EQ(entries.size(), 2u);
  session = makeSession();
  ASSERT_EQ(session->erase(path.c_str(), 4, original, entries), TintaJournalResult::Ok);
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].identity, added.identity);
  const auto before = inventory_hal_test::state.files;
  session = makeSession();
  ASSERT_EQ(session->restore(path.c_str(), 4, entries), TintaJournalResult::Ok);
  EXPECT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].identity, added.identity);
  EXPECT_EQ(inventory_hal_test::state.files, before);
  EXPECT_FALSE(session->requiresRecovery());
}
TEST_F(BookmarkFileTest, ReaderLifecycleRetainsDisplayedListUntilUncertainPublicationRecovers) {
  using namespace companion;
  auto& state = inventory_hal_test::state;
  state.enumerateFileMap = true;
  ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
  install(R"({"bookmarks":[{"name":"Original","si":1,"vo":2}]})");
  const auto path = BookmarkUtil::getBookmarkPath(BOOK);
  Digest edition{};
  edition.fill(7);
  Identity generation{};
  generation.fill(2);
  MigrationIdentities identities;
  std::array<uint8_t, 128> scratch{};
  const auto makeSession = [&] {
    return makeUniqueNoThrow<NativeBookmarkReaderSession>(
        scratch, edition, generation, identities, +[](void*) { return true; },
        +[](void*, const BookmarkMigrationClaim&) { return true; },
        +[](void*, const BookmarkPublicationClaim&) { return true; },
        +[](void*, const BookmarkPreparationClaim&) { return true; }, nullptr);
  };
  std::vector<BookmarkEntry> entries;
  auto session = makeSession();
  ASSERT_EQ(session->restore(path.c_str(), 4, entries), TintaJournalResult::Ok);
  const auto selected = entries[0];
  const auto original = state.files.at(path);
  state.failSyncPath = HalBookmarkPublicationPaths::INTENT_NEXT;
  session = makeSession();
  EXPECT_EQ(session->rename(path.c_str(), 4, selected, "Renamed", entries), TintaJournalResult::IoError);
  EXPECT_TRUE(session->requiresRecovery());
  EXPECT_EQ(entries[0].name, "Original");
  EXPECT_EQ(state.files.at(path), original);
  session.reset();
  state.failSyncPath.clear();
  session = makeSession();
  ASSERT_EQ(session->restore(path.c_str(), 4, entries), TintaJournalResult::Ok);
  EXPECT_EQ(entries[0].name, "Renamed");
  EXPECT_EQ(entries[0].identity, selected.identity);
  EXPECT_FALSE(session->requiresRecovery());
}

TEST_F(BookmarkFileTest, ReaderLifecycleReleasesLegacyWorkspaceBeforePublication) {
  using namespace companion;
  inventory_hal_test::state.enumerateFileMap = true;
  ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
  install(R"({"bookmarks":[{"name":"Original","si":1,"vo":2}]})");
  const auto path = BookmarkUtil::getBookmarkPath(BOOK);
  const auto original = inventory_hal_test::state.files.at(path);
  Digest edition{};
  edition.fill(7);
  Identity generation{};
  generation.fill(2);
  MigrationIdentities identities;
  std::array<uint8_t, 128> scratch{};
  struct Context {
    bool valid = true;
    unsigned releases = 0;
  } context;
  auto session = makeUniqueNoThrow<NativeBookmarkReaderSession>(
      scratch, edition, generation, identities, +[](void* ctx) { return static_cast<Context*>(ctx)->valid; },
      +[](void* ctx, const BookmarkMigrationClaim&) { return static_cast<Context*>(ctx)->valid; },
      +[](void* ctx, const BookmarkPublicationClaim&) { return static_cast<Context*>(ctx)->valid; },
      +[](void* ctx, const BookmarkPreparationClaim&) { return static_cast<Context*>(ctx)->valid; }, &context,
      +[](void* ctx) {
        auto& c = *static_cast<Context*>(ctx);
        ++c.releases;
        c.valid = false;
      });
  std::vector<BookmarkEntry> entries;
  EXPECT_EQ(session->restore(path.c_str(), 4, entries), TintaJournalResult::IoError);
  EXPECT_TRUE(entries.empty());
  EXPECT_EQ(context.releases, 1u);
  EXPECT_TRUE(session->requiresRecovery());
  EXPECT_EQ(inventory_hal_test::state.files.at(path), original);
  EXPECT_FALSE(inventory_hal_test::state.files.contains(HalBookmarkJsonStage::PATH));
}

TEST(BookmarkPageMatch, ExactOffsetsUseSpineAndExclusiveNextPageBoundary) {
  BookmarkEntry entry{};
  entry.hasVisibleTextOffset = true;
  entry.computedSpineIndex = 2;
  entry.computedChapterPageCount = 99;
  entry.computedChapterProgress = 5;
  entry.percentage = 1.0f;
  const BookmarkTextPageRange range{100, 200, false};
  for (const auto offset : {99u, 100u, 175u, 199u, 200u}) {
    entry.visibleTextOffset = offset;
    EXPECT_EQ(bookmarkMatchesTextPage(entry, 2, range), offset >= 100 && offset < 200);
    EXPECT_FALSE(bookmarkMatchesTextPage(entry, 1, range));
  }
  entry.visibleTextOffset = 200;
  EXPECT_TRUE(bookmarkMatchesTextPage(entry, 2, {200, 300, false}));
  entry.visibleTextOffset = 175;
  EXPECT_TRUE(bookmarkMatchesTextPage(entry, 2, {150, 180, false}));
}
TEST(BookmarkPageMatch, PartialWatermarkDoesNotClaimUnseenChapterTail) {
  BookmarkEntry entry{};
  entry.hasVisibleTextOffset = true;
  entry.computedSpineIndex = 2;
  entry.visibleTextOffset = 100;
  EXPECT_TRUE(bookmarkMatchesTextPage(entry, 2, {100, std::nullopt, false}));
  entry.visibleTextOffset = 150;
  EXPECT_FALSE(bookmarkMatchesTextPage(entry, 2, {100, std::nullopt, false}));
  EXPECT_TRUE(bookmarkMatchesTextPage(entry, 2, {100, std::nullopt, true}));
  EXPECT_FALSE(bookmarkMatchesTextPage(entry, 2, {std::nullopt, std::nullopt, true}));
  entry.visibleTextOffset = 100;
  EXPECT_FALSE(bookmarkMatchesTextPage(entry, 2, {100, 100, false}));
  EXPECT_FALSE(bookmarkMatchesTextPage(entry, 2, {100, 90, false}));
  EXPECT_FALSE(bookmarkMatchesTextPage(entry, -1, {100, 200, false}));
  entry.hasVisibleTextOffset = false;
  EXPECT_FALSE(bookmarkMatchesTextPage(entry, 2, {100, 200, false}));
}

TEST_F(BookmarkFileTest, AsciiPreparationHashRemainsCompatibleWithExistingClaims) {
  using namespace companion;
  ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
  const auto claim = halPreparationClaim();
  auto session = makeUniqueNoThrow<NativeBookmarkPreparationSession>(
      +[](void*, const BookmarkPreparationClaim&) { return true; }, nullptr);
  ASSERT_EQ(
      session->begin("/.crosspoint/bookmarks/BOOK.JSON", claim.edition, claim.transaction, claim.storageGeneration),
      TintaJournalResult::Ok);
  static constexpr unsigned char EXPECTED[] = "lila-bookmark-preparation-path-v1/.crosspoint/bookmarks/book.json";
  Digest hash{};
  ASSERT_EQ(mbedtls_sha256(EXPECTED, sizeof(EXPECTED) - 1, hash.data(), 0), 0);
  EXPECT_EQ(session->preparationClaim().destinationPathHash, hash);
}

TEST_F(BookmarkFileTest, UnicodeMigrationCaseVariantReusesOriginalClaimAndBackup) {
  using namespace companion;
  auto& state = inventory_hal_test::state;
  constexpr const char* SOURCE = "/.crosspoint/bookmarks/café-📖.json";
  state.files[SOURCE] = {'o', 'l', 'd'};
  std::array<uint8_t, 128> scratch{};
  Digest edition{};
  Identity transaction{}, generation{};
  edition.fill(7);
  transaction.fill(1);
  generation.fill(2);
  const auto validator = +[](void*, const BookmarkMigrationClaim&) { return true; };
  auto session = makeUniqueNoThrow<HalBookmarkMigrationBackupSession>(scratch, validator, nullptr);
  ASSERT_EQ(session->prepare(SOURCE, edition, transaction, generation), TintaJournalResult::Ok);
  const auto claim = session->migrationClaim();
  const std::string backup = session->backupPath();
  const auto retained = state.files;
  session.reset();
  transaction.fill(9);
  session = makeUniqueNoThrow<HalBookmarkMigrationBackupSession>(scratch, validator, nullptr);
  ASSERT_EQ(session->prepare("/.crosspoint/bookmarks/CAFÉ-📖.JSON", edition, transaction, generation),
            TintaJournalResult::Ok);
  EXPECT_EQ(session->migrationClaim(), claim);
  EXPECT_STREQ(session->backupPath(), backup.c_str());
  EXPECT_EQ(state.files, retained);
}

TEST_F(BookmarkFileTest, UnicodeReaderLifecycleMigratesAndReplaysStableBookmark) {
  using namespace companion;
  auto& state = inventory_hal_test::state;
  state.enumerateFileMap = true;
  ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
  ASSERT_TRUE(Storage.ensureDirectoryExists(HalBookmarkPublicationPaths::ACTIVE_PARENT));
  constexpr const char* PATH = "/.crosspoint/bookmarks/café-📖.json";
  const std::string legacy = R"({"bookmarks":[{"name":"Original","summary":"Legacy","si":2,"vo":42}]})";
  state.files[PATH] = {legacy.begin(), legacy.end()};
  Digest edition{};
  edition.fill(7);
  Identity generation{};
  generation.fill(2);
  MigrationIdentities identities;
  std::array<uint8_t, 128> scratch{};
  const auto makeSession = [&] {
    return makeUniqueNoThrow<NativeBookmarkReaderSession>(
        scratch, edition, generation, identities, +[](void*) { return true; },
        +[](void*, const BookmarkMigrationClaim&) { return true; },
        +[](void*, const BookmarkPublicationClaim&) { return true; },
        +[](void*, const BookmarkPreparationClaim&) { return true; }, nullptr);
  };
  std::vector<BookmarkEntry> entries;
  auto session = makeSession();
  ASSERT_EQ(session->restore(PATH, 4, entries), TintaJournalResult::Ok);
  ASSERT_EQ(entries.size(), 1u);
  const auto identity = entries[0].identity;
  ASSERT_TRUE(BookmarkIdentity::valid(identity));
  const auto retained = state.files;
  session.reset();
  session = makeSession();
  ASSERT_EQ(session->restore(PATH, 4, entries), TintaJournalResult::Ok);
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].identity, identity);
  EXPECT_EQ(state.files, retained);
}

TEST_F(BookmarkFileTest, ReaderRestoreExposesConcurrentIdentityAndPublishesOnlyAfterExplicitChoice) {
  using namespace companion;
  auto& state = inventory_hal_test::state;
  state.enumerateFileMap = true;
  ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
  install(R"({"bookmarks":[{"id":"03000000000000000000000000000000","name":"Old","si":1,"vo":42}]})");
  const auto path = BookmarkUtil::getBookmarkPath(BOOK);
  const auto original = state.files.at(path);
  Digest edition{};
  edition.fill(7);
  Identity generation{};
  generation.fill(2);
  MigrationIdentities identities;
  std::array<uint8_t, 128> scratch{};
  HalTintaJournalStorage storage;
  std::array<uint8_t, 1024> journalScratch{};
  TintaJournal journal(storage, journalScratch);
  ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
  SyncEvent event;
  event.identity.epoch = event.identity.sequence = 1;
  event.storageGeneration = generation;
  event.resource = edition;
  event.kind = EventKind::BookmarkPut;
  BookmarkBodyView bookmark;
  bookmark.identity[0] = 3;
  std::array<uint8_t, MAX_BOOKMARK_BODY_SIZE> body{};
  for (uint8_t origin = 1; origin <= 2; ++origin) {
    event.identity.origin.fill(origin);
    for (uint32_t sequence = 1; sequence <= 2; ++sequence) {
      event.identity.sequence = sequence;
      bookmark.identity[0] = sequence == 1 ? 3 : 7;
      bookmark.anchor = {static_cast<uint16_t>(sequence), static_cast<uint32_t>(origin * 10)};
      const auto size = encodeBookmarkBody(bookmark, body);
      ASSERT_TRUE(storage.digest(std::span(body).first(size), event.bodyHash));
      ASSERT_EQ(journal.append(event, std::span(body).first(size)), TintaJournalResult::Ok);
    }
  }
  ASSERT_TRUE(storage.close());
  bookmark.identity[0] = 3;
  const auto makeSession = [&] {
    return makeUniqueNoThrow<NativeBookmarkReaderSession>(
        scratch, edition, generation, identities, +[](void*) { return true; },
        +[](void*, const BookmarkMigrationClaim&) { return true; },
        +[](void*, const BookmarkPublicationClaim&) { return true; },
        +[](void*, const BookmarkPreparationClaim&) { return true; }, nullptr);
  };
  std::vector<BookmarkEntry> entries;
  auto session = makeSession();
  ASSERT_EQ(session->restore(path.c_str(), 4, entries), TintaJournalResult::Conflict);
  EXPECT_TRUE(entries.empty());
  EXPECT_EQ(session->conflictIdentity(), bookmark.identity);
  EXPECT_TRUE(session->requiresRecovery());
  EXPECT_EQ(state.files.at(path), original);
  EXPECT_FALSE(state.files.contains(HalBookmarkPublicationPaths::INTENT));
  session = makeSession();
  ASSERT_EQ(session->prepareChoice(path.c_str(), 4), TintaJournalResult::Ok);
  EXPECT_FALSE(session->requiresRecovery());
  auto choices = makeUniqueNoThrow<NativeBookmarkChoicePage>();
  ASSERT_TRUE(choices);
  ASSERT_EQ(choices->load(edition, bookmark.identity, 4), TintaJournalResult::Ok);
  EXPECT_TRUE(choices->matches(edition, bookmark.identity));
  auto different = edition;
  different[0] ^= 1;
  EXPECT_FALSE(choices->matches(different, bookmark.identity));
  ASSERT_EQ(choices->resolve(0, identities), TintaJournalResult::Ok);
  EXPECT_EQ(state.files.at(path), original);
  session = makeSession();
  ASSERT_EQ(session->restore(path.c_str(), 4, entries), TintaJournalResult::Conflict);
  EXPECT_EQ(session->conflictIdentity()[0], 7u);
  EXPECT_TRUE(entries.empty());
  EXPECT_EQ(state.files.at(path), original);
  bookmark.identity[0] = 7;
  session = makeSession();
  ASSERT_EQ(session->prepareChoice(path.c_str(), 4), TintaJournalResult::Ok);
  ASSERT_EQ(choices->load(edition, bookmark.identity, 4), TintaJournalResult::Ok);
  ASSERT_EQ(choices->resolve(0, identities), TintaJournalResult::Ok);
  EXPECT_EQ(state.files.at(path), original);
  session = makeSession();
  ASSERT_EQ(session->restore(path.c_str(), 4, entries), TintaJournalResult::Ok);
  ASSERT_EQ(entries.size(), 2u);
  EXPECT_EQ(entries[0].identity[0], 3u);
  EXPECT_EQ(entries[1].identity, bookmark.identity);
  EXPECT_EQ(entries[0].visibleTextOffset, 20u);
  EXPECT_EQ(entries[1].visibleTextOffset, 20u);
  EXPECT_FALSE(tinta_body_detail::nonzero(session->conflictIdentity()));
  EXPECT_FALSE(session->requiresRecovery());
  const auto retained = state.files;
  session = makeSession();
  ASSERT_EQ(session->restore(path.c_str(), 4, entries), TintaJournalResult::Ok);
  EXPECT_EQ(state.files, retained);
}

TEST_F(BookmarkFileTest, CanonicalLegacyDecisionSurvivesMultipleConcurrentBookmarkChoices) {
  using namespace companion;
  for (const auto decision : {LegacyBookmarkDecision::Associate, LegacyBookmarkDecision::LeaveUnassociated}) {
    inventory_hal_test::state = {};
    auto& state = inventory_hal_test::state;
    state.enumerateFileMap = true;
    ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
    install(R"({"bookmarks":[{"id":"03000000000000000000000000000000","name":"Old","si":1,"vo":42}]})");
    const auto path = BookmarkUtil::getBookmarkPath(BOOK);
    const auto original = state.files.at(path);
    Digest edition{};
    edition.fill(7);
    std::array<char, 128> canonical{};
    ASSERT_TRUE(BookmarkUtil::getBookmarkEditionPath(edition, canonical));
    Identity generation{};
    generation.fill(2);
    MigrationIdentities identities;
    std::array<uint8_t, 128> scratch{};
    HalTintaJournalStorage storage;
    std::array<uint8_t, 1024> journalScratch{};
    TintaJournal journal(storage, journalScratch);
    ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
    SyncEvent event;
    event.identity.epoch = event.identity.sequence = 1;
    event.storageGeneration = generation;
    event.resource = edition;
    event.kind = EventKind::BookmarkPut;
    BookmarkBodyView bookmark;
    bookmark.identity[0] = 3;
    std::array<uint8_t, MAX_BOOKMARK_BODY_SIZE> body{};
    for (uint8_t origin = 1; origin <= 2; ++origin) {
      event.identity.origin.fill(origin);
      for (uint32_t sequence = 1; sequence <= 2; ++sequence) {
        event.identity.sequence = sequence;
        bookmark.identity[0] = sequence == 1 ? 3 : 7;
        bookmark.anchor = {static_cast<uint16_t>(sequence), static_cast<uint32_t>(origin * 10)};
        const auto size = encodeBookmarkBody(bookmark, body);
        ASSERT_TRUE(storage.digest(std::span(body).first(size), event.bodyHash));
        ASSERT_EQ(journal.append(event, std::span(body).first(size)), TintaJournalResult::Ok);
      }
    }
    ASSERT_TRUE(storage.close());
    bookmark.identity[0] = 3;
    const auto makeSession = [&] {
      return makeUniqueNoThrow<NativeBookmarkReaderSession>(
          scratch, edition, generation, identities, +[](void*) { return true; },
          +[](void*, const BookmarkMigrationClaim&) { return true; },
          +[](void*, const BookmarkPublicationClaim&) { return true; },
          +[](void*, const BookmarkPreparationClaim&) { return true; }, nullptr);
    };
    std::vector<BookmarkEntry> entries;
    auto session = makeSession();
    ASSERT_EQ(session->restoreEdition(canonical.data(), path.c_str(), 4, entries, decision),
              TintaJournalResult::Conflict);
    EXPECT_TRUE(entries.empty());
    EXPECT_EQ(session->conflictIdentity(), bookmark.identity);
    EXPECT_TRUE(session->requiresRecovery());
    EXPECT_EQ(state.files.at(path), original);
    EXPECT_FALSE(state.files.contains(HalBookmarkPublicationPaths::INTENT));
    session = makeSession();
    ASSERT_EQ(session->prepareChoice(canonical.data(), 4), TintaJournalResult::Ok);
    EXPECT_FALSE(session->requiresRecovery());
    auto choices = makeUniqueNoThrow<NativeBookmarkChoicePage>();
    ASSERT_TRUE(choices);
    ASSERT_EQ(choices->load(edition, bookmark.identity, 4), TintaJournalResult::Ok);
    EXPECT_TRUE(choices->matches(edition, bookmark.identity));
    auto different = edition;
    different[0] ^= 1;
    EXPECT_FALSE(choices->matches(different, bookmark.identity));
    ASSERT_EQ(choices->resolve(0, identities), TintaJournalResult::Ok);
    EXPECT_EQ(state.files.at(path), original);
    session = makeSession();
    ASSERT_EQ(session->restoreEdition(canonical.data(), path.c_str(), 4, entries, decision),
              TintaJournalResult::Conflict);
    EXPECT_EQ(session->conflictIdentity()[0], 7u);
    EXPECT_TRUE(entries.empty());
    EXPECT_EQ(state.files.at(path), original);
    bookmark.identity[0] = 7;
    session = makeSession();
    ASSERT_EQ(session->prepareChoice(canonical.data(), 4), TintaJournalResult::Ok);
    ASSERT_EQ(choices->load(edition, bookmark.identity, 4), TintaJournalResult::Ok);
    ASSERT_EQ(choices->resolve(0, identities), TintaJournalResult::Ok);
    EXPECT_EQ(state.files.at(path), original);
    session = makeSession();
    ASSERT_EQ(session->restoreEdition(canonical.data(), path.c_str(), 4, entries, decision), TintaJournalResult::Ok);
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].identity[0], 3u);
    EXPECT_EQ(entries[1].identity, bookmark.identity);
    EXPECT_EQ(entries[0].visibleTextOffset, 20u);
    EXPECT_EQ(entries[1].visibleTextOffset, 20u);
    EXPECT_FALSE(tinta_body_detail::nonzero(session->conflictIdentity()));
    EXPECT_FALSE(session->requiresRecovery());
    const auto retained = state.files;
    session = makeSession();
    ASSERT_EQ(session->restoreEdition(canonical.data(), path.c_str(), 4, entries, decision), TintaJournalResult::Ok);
    EXPECT_EQ(state.files, retained);
  }
}

TEST_F(BookmarkFileTest, ChoicePreflightProtectsCorruptPublicationInsteadOfExposingABookmarkConflict) {
  using namespace companion;
  auto& state = inventory_hal_test::state;
  state.enumerateFileMap = true;
  ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
  state.files[HalBookmarkPublicationPaths::INTENT] = {9};
  Digest edition{};
  edition.fill(7);
  Identity generation{};
  generation.fill(2);
  MigrationIdentities identities;
  std::array<uint8_t, 128> scratch{};
  auto session = makeUniqueNoThrow<NativeBookmarkReaderSession>(
      scratch, edition, generation, identities, +[](void*) { return true; },
      +[](void*, const BookmarkMigrationClaim&) { return true; },
      +[](void*, const BookmarkPublicationClaim&) { return true; },
      +[](void*, const BookmarkPreparationClaim&) { return true; }, nullptr);
  ASSERT_TRUE(session);
  const auto retained = state.files;
  const auto path = BookmarkUtil::getBookmarkPath(BOOK);
  EXPECT_EQ(session->prepareChoice(path.c_str(), 4), TintaJournalResult::Corrupt);
  EXPECT_TRUE(session->requiresRecovery());
  EXPECT_FALSE(tinta_body_detail::nonzero(session->conflictIdentity()));
  EXPECT_EQ(state.files, retained);
}

TEST_F(BookmarkFileTest, ForeignEditionCacheCannotPublishRowsEvenWhenLegacyParsingSucceeds) {
  using namespace companion;
  auto& state = inventory_hal_test::state;
  state.enumerateFileMap = true;
  ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
  install(R"({"bookmarks":[{"id":"03000000000000000000000000000000","name":"Foreign","si":1,"vo":42}]})");
  const auto path = BookmarkUtil::getBookmarkPath(BOOK);
  const auto original = state.files.at(path);
  Digest edition{}, foreign{};
  edition.fill(7);
  foreign.fill(9);
  Identity generation{};
  generation.fill(2);
  MigrationIdentities identities;
  BookmarkBodyView bookmark;
  bookmark.identity[0] = 3;
  auto save = makeUniqueNoThrow<NativeBookmarkSaveSession>();
  ASSERT_TRUE(save);
  ASSERT_EQ(save->persist(bookmark, foreign, identities), TintaJournalResult::Ok);
  save.reset();
  const auto journal = state.files.at(TINTA_JOURNAL_EVENTS);
  std::array<uint8_t, 128> scratch{};
  auto session = makeUniqueNoThrow<NativeBookmarkReaderSession>(
      scratch, edition, generation, identities, +[](void*) { return true; },
      +[](void*, const BookmarkMigrationClaim&) { return true; },
      +[](void*, const BookmarkPublicationClaim&) { return true; },
      +[](void*, const BookmarkPreparationClaim&) { return true; }, nullptr);
  ASSERT_TRUE(session);
  std::vector<BookmarkEntry> entries;
  EXPECT_EQ(session->restore(path.c_str(), 4, entries), TintaJournalResult::Conflict);
  EXPECT_TRUE(entries.empty());
  EXPECT_TRUE(session->requiresRecovery());
  EXPECT_FALSE(tinta_body_detail::nonzero(session->conflictIdentity()));
  EXPECT_EQ(state.files.at(path), original);
  EXPECT_EQ(state.files.at(TINTA_JOURNAL_EVENTS), journal);
}

TEST(BookmarkPath, EditionNamespaceSeparatesDifferentEditionsAndLegacyFlatteningCollisions) {
  companion::Digest first{}, second{};
  first.fill(0x12);
  second.fill(0x34);
  std::array<char, 128> firstPath{}, secondPath{};
  ASSERT_EQ(BookmarkUtil::getBookmarkPath("/books/a_b.epub"), BookmarkUtil::getBookmarkPath("/books/a/b.epub"));
  ASSERT_TRUE(BookmarkUtil::getBookmarkEditionPath(first, firstPath));
  ASSERT_TRUE(BookmarkUtil::getBookmarkEditionPath(second, secondPath));
  EXPECT_STRNE(firstPath.data(), secondPath.data());
  EXPECT_STREQ(firstPath.data(),
               "/.crosspoint/bookmarks/editions/1212121212121212121212121212121212121212121212121212121212121212.json");
  EXPECT_NE(firstPath.data(), BookmarkUtil::getBookmarkPath(
                                  "/editions/1212121212121212121212121212121212121212121212121212121212121212.epub"));
}

TEST(BookmarkPath, EditionPathRejectsInvalidProofAndSmallBuffersWithoutChangingOutput) {
  std::array<char, 128> output{};
  output.fill('x');
  const auto original = output;
  companion::Digest edition{};
  EXPECT_FALSE(BookmarkUtil::getBookmarkEditionPath(edition, output));
  edition[0] = 1;
  EXPECT_FALSE(BookmarkUtil::getBookmarkEditionPath(std::span(edition).first(31), output));
  EXPECT_FALSE(BookmarkUtil::getBookmarkEditionPath(edition, std::span(output).first(2)));
  EXPECT_EQ(output, original);
  ASSERT_TRUE(BookmarkUtil::getBookmarkEditionPath(edition, output));
  const auto size = strlen(output.data());
  output = original;
  EXPECT_FALSE(BookmarkUtil::getBookmarkEditionPath(edition, std::span(output).first(size)));
  EXPECT_EQ(output, original);
  ASSERT_TRUE(BookmarkUtil::getBookmarkEditionPath(edition, std::span(output).first(size + 1)));
  EXPECT_EQ(output[size], 0);
  EXPECT_EQ(output[size + 1], 'x');
}

TEST(BookmarkSummary, ClipsAtCompleteUtf8Characters) {
  const std::pair<std::string, std::string> cases[] = {
      {std::string(71, 'a') + "é", std::string(71, 'a')},
      {std::string(70, 'a') + "é", std::string(70, 'a') + "é"},
      {std::string(70, 'a') + "📚", std::string(70, 'a')},
      {std::string(68, 'a') + "📚more", std::string(68, 'a') + "📚"},
      {std::string(72, 'a'), std::string(72, 'a')},
  };
  for (const auto& [input, expected] : cases) {
    const auto summary = BookmarkUtil::sanitizeBookmarkSummary(input);
    EXPECT_EQ(summary, expected);
    EXPECT_LE(summary.size(), 72u);
    EXPECT_TRUE(
        companion::reading_body_detail::text({reinterpret_cast<const uint8_t*>(summary.data()), summary.size()}));
  }
}

TEST(BookmarkSummary, PreservesMultilingualTextWhileCleaningWhitespace) {
  EXPECT_EQ(BookmarkUtil::sanitizeBookmarkSummary("  café  日本語 📚 \t "), "café 日本語 📚");
}

TEST(BookmarkPath, BoundedPathBuilderPreservesLegacyNamesAndUnicode) {
  std::array<char, 512> output{};
  for (const std::string path : {"/book.epub", "/books/café-📖.epub", "/dir.with.dots/book", "/a/b/c.epub", "/.epub"}) {
    ASSERT_TRUE(BookmarkUtil::getBookmarkPath(path, output));
    EXPECT_STREQ(output.data(), BookmarkUtil::getBookmarkPath(path).c_str());
  }
}
TEST(BookmarkPath, BoundedPathBuilderRejectsBadInputsWithoutChangingOutput) {
  std::array<char, 512> output;
  output.fill('x');
  const auto untouched = output;
  EXPECT_FALSE(BookmarkUtil::getBookmarkPath("relative.epub", output));
  EXPECT_FALSE(BookmarkUtil::getBookmarkPath("/", output));
  EXPECT_FALSE(BookmarkUtil::getBookmarkPath(std::string_view("/a\0b.epub", 9), output));
  EXPECT_FALSE(BookmarkUtil::getBookmarkPath("/" + std::string(512, 'a') + ".epub", output));
  EXPECT_FALSE(BookmarkUtil::getBookmarkPath("/book.epub", std::span(output).first(2)));
  EXPECT_EQ(output, untouched);
  const auto expected = BookmarkUtil::getBookmarkPath("/book.epub");
  EXPECT_FALSE(BookmarkUtil::getBookmarkPath("/book.epub", std::span(output).first(expected.size())));
  EXPECT_EQ(output, untouched);
  ASSERT_TRUE(BookmarkUtil::getBookmarkPath("/book.epub", std::span(output).first(expected.size() + 1)));
  EXPECT_STREQ(output.data(), expected.c_str());
}

TEST_F(BookmarkFileTest, MatchingPageDeletionJournalsAllMatchesBeforeSinglePublication) {
  using namespace companion;
  auto& state = inventory_hal_test::state;
  state.enumerateFileMap = true;
  ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
  install(R"({"bookmarks":[{"name":"A","si":2,"vo":42},{"name":"B","si":2,"vo":42},{"name":"C","si":3,"vo":17}]})");
  const auto path = BookmarkUtil::getBookmarkPath(BOOK);
  Digest edition{};
  edition.fill(7);
  Identity generation{};
  generation.fill(2);
  MigrationIdentities identities;
  std::array<uint8_t, 128> scratch{};
  const auto makeSession = [&] {
    return makeUniqueNoThrow<NativeBookmarkReaderSession>(
        scratch, edition, generation, identities, +[](void*) { return true; },
        +[](void*, const BookmarkMigrationClaim&) { return true; },
        +[](void*, const BookmarkPublicationClaim&) { return true; },
        +[](void*, const BookmarkPreparationClaim&) { return true; }, nullptr);
  };
  std::vector<BookmarkEntry> entries;
  auto session = makeSession();
  ASSERT_EQ(session->restore(path.c_str(), 4, entries), TintaJournalResult::Ok);
  ASSERT_EQ(entries.size(), 3u);
  const auto original = state.files.at(path);
  struct MatchContext {
    std::vector<BookmarkEntry>* entries;
    unsigned matches = 0;
  } context{&entries};
  const auto matches = +[](void* raw, const BookmarkEntry& entry) {
    auto& context = *static_cast<MatchContext*>(raw);
    EXPECT_EQ(context.entries->size(), 3u);
    const bool selected = entry.computedSpineIndex == 2;
    if (selected) ++context.matches;
    return selected;
  };
  session = makeSession();
  ASSERT_EQ(session->eraseMatching(path.c_str(), 4, entries, matches, &context), TintaJournalResult::Ok);
  EXPECT_EQ(context.matches, 2u);
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].name, "C");
  EXPECT_NE(state.files.at(path), original);
  const auto retained = state.files;
  session = makeSession();
  ASSERT_EQ(session->restore(path.c_str(), 4, entries), TintaJournalResult::Ok);
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].name, "C");
  EXPECT_EQ(state.files, retained);
}

TEST_F(BookmarkFileTest, MatchingDeletionRecoversCommittedDeletesAfterFailedPublication) {
  using namespace companion;
  auto& state = inventory_hal_test::state;
  state.enumerateFileMap = true;
  ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
  install(R"({"bookmarks":[{"name":"A","si":2,"vo":42},{"name":"B","si":2,"vo":42}]})");
  const auto path = BookmarkUtil::getBookmarkPath(BOOK);
  Digest edition{};
  edition.fill(7);
  Identity generation{};
  generation.fill(2);
  MigrationIdentities identities;
  std::array<uint8_t, 128> scratch{};
  const auto makeSession = [&] {
    return makeUniqueNoThrow<NativeBookmarkReaderSession>(
        scratch, edition, generation, identities, +[](void*) { return true; },
        +[](void*, const BookmarkMigrationClaim&) { return true; },
        +[](void*, const BookmarkPublicationClaim&) { return true; },
        +[](void*, const BookmarkPreparationClaim&) { return true; }, nullptr);
  };
  std::vector<BookmarkEntry> entries;
  auto session = makeSession();
  ASSERT_EQ(session->restore(path.c_str(), 4, entries), TintaJournalResult::Ok);
  const auto original = state.files.at(path);
  state.failSyncPath = HalBookmarkPublicationPaths::INTENT_NEXT;
  session = makeSession();
  EXPECT_EQ(session->eraseMatching(
                path.c_str(), 4, entries, +[](void*, const BookmarkEntry&) { return true; }, nullptr),
            TintaJournalResult::IoError);
  EXPECT_TRUE(session->requiresRecovery());
  EXPECT_EQ(entries.size(), 2u);
  EXPECT_EQ(state.files.at(path), original);
  state.failSyncPath.clear();
  session = makeSession();
  ASSERT_EQ(session->restore(path.c_str(), 4, entries), TintaJournalResult::Ok);
  EXPECT_TRUE(entries.empty());
  const auto retained = state.files;
  session = makeSession();
  ASSERT_EQ(session->restore(path.c_str(), 4, entries), TintaJournalResult::Ok);
  EXPECT_TRUE(entries.empty());
  EXPECT_EQ(state.files, retained);
}
