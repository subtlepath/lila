#include <Memory.h>
#include <PersistableStore.h>
#include <gtest/gtest.h>

#include "HalBookmarkJsonStage.h"
#include "lib/Companion/CompanionBookmarkJsonWriter.h"
#include "src/util/BookmarkFile.h"
#include "src/util/BookmarkUtil.h"

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
