#include <gtest/gtest.h>

#include "lib/hal/HalCompanionFileLookup.h"
using namespace companion;
namespace {
class CompanionFileLookupTest : public testing::Test {
 protected:
  HalCompanionFileLookup lookup;
  void SetUp() override {
    inventory_hal_test::state = {};
    Storage.ensureDirectoryExists(TRANSFER_DIRECTORY);
  }
};
TEST_F(CompanionFileLookupTest, RequiresSuccessfulEnumerationToProveAbsenceAndReusesHandles) {
  auto& state = inventory_hal_test::state;
  constexpr char TARGET[] = "/.crosspoint/companion/missing";
  EXPECT_EQ(lookup.inspect(TARGET), CompanionFilePresence::Missing);
  EXPECT_EQ(state.preparations, 2u);
  state.directoryErrorPath = TRANSFER_DIRECTORY;
  EXPECT_EQ(lookup.inspect(TARGET), CompanionFilePresence::Error);
  state.directoryErrorPath.clear();
  state.files[TARGET] = {1};
  EXPECT_EQ(lookup.inspect(TARGET), CompanionFilePresence::Present);
  state.files.clear();
  EXPECT_EQ(lookup.inspect(TARGET), CompanionFilePresence::Missing);
  EXPECT_EQ(state.preparations, 2u);
}
TEST_F(CompanionFileLookupTest, JournalParentRequiresCheckedAbsenceAndRestrictsDirectChildren) {
  static constexpr char PARENT[] = "/.crosspoint/companion/tinta-events";
  static constexpr char HEADER[] = "/.crosspoint/companion/tinta-events/header-a.bin";
  ASSERT_TRUE(Storage.ensureDirectoryExists(PARENT));
  inventory_hal_test::state.directories[PARENT] = {};
  inventory_hal_test::state.enumerateFileMap = true;
  HalCompanionFileLookup journalLookup(nullptr, nullptr, PARENT);
  EXPECT_EQ(journalLookup.inspect(HEADER), CompanionFilePresence::Missing);
  inventory_hal_test::state.directoryErrorPath = PARENT;
  EXPECT_EQ(journalLookup.inspect(HEADER), CompanionFilePresence::Error);
  inventory_hal_test::state.directoryErrorPath.clear();
  inventory_hal_test::state.files[HEADER] = {1};
  EXPECT_EQ(journalLookup.inspect(HEADER), CompanionFilePresence::Present);
  EXPECT_EQ(journalLookup.inspect("/.crosspoint/companion/header-a.bin"), CompanionFilePresence::Error);
  EXPECT_EQ(journalLookup.inspect("/.crosspoint/companion/tinta-events/nested/header-a.bin"),
            CompanionFilePresence::Error);
}
TEST_F(CompanionFileLookupTest, FilesDirectoriesAndAsciiCaseUseFatNameSemantics) {
  auto& state = inventory_hal_test::state;
  state.files["/.crosspoint/companion/DiCtIoNaRy-ABC.zip"] = {1};
  state.directories["/.crosspoint/companion/collision"] = {};
  EXPECT_EQ(lookup.inspect("/.crosspoint/companion/dictionary-abc.ZIP"), CompanionFilePresence::Present);
  EXPECT_EQ(lookup.inspect("/.crosspoint/companion/collision"), CompanionFilePresence::Present);
  EXPECT_EQ(lookup.inspect("/.crosspoint/companion/dictionary-abcd.zip"), CompanionFilePresence::Missing);
}
TEST_F(CompanionFileLookupTest, OpenCloseNameAndParentFailuresNeverReportMissing) {
  auto& state = inventory_hal_test::state;
  constexpr char TARGET[] = "/.crosspoint/companion/missing";
  state.failOpen = true;
  EXPECT_EQ(lookup.inspect(TARGET), CompanionFilePresence::Error);
  state.failOpen = false;
  state.failClosePath = TRANSFER_DIRECTORY;
  EXPECT_EQ(lookup.inspect(TARGET), CompanionFilePresence::Error);
  state.failClosePath.clear();
  state.files["/.crosspoint/companion/" + std::string(512, 'a')] = {};
  EXPECT_EQ(lookup.inspect(TARGET), CompanionFilePresence::Error);
  state.files.clear();
  state.directories.erase(TRANSFER_DIRECTORY);
  EXPECT_EQ(lookup.inspect(TARGET), CompanionFilePresence::Error);
}
TEST_F(CompanionFileLookupTest, FatShortNameAliasProtectsExistingFileAndDirectory) {
  auto& state = inventory_hal_test::state;
  constexpr char FILE[] = "/.crosspoint/companion/a long filename.json";
  constexpr char DIRECTORY[] = "/.crosspoint/companion/a long directory";
  state.files[FILE] = {1};
  state.directories[DIRECTORY] = {};
  state.aliases[FILE] = "ALONGF~1.JSO";
  state.aliases[DIRECTORY] = "ALONGD~1";
  EXPECT_EQ(lookup.inspect("/.crosspoint/companion/alongf~1.jso"), CompanionFilePresence::Present);
  EXPECT_EQ(lookup.inspect("/.crosspoint/companion/alongd~1"), CompanionFilePresence::Present);
  EXPECT_EQ(lookup.inspect("/.crosspoint/companion/alongf~2.jso"), CompanionFilePresence::Missing);
}
TEST_F(CompanionFileLookupTest, FailedAliasReadCannotProveAbsence) {
  auto& state = inventory_hal_test::state;
  state.files["/.crosspoint/companion/a long filename.json"] = {1};
  state.failShortName = true;
  EXPECT_EQ(lookup.inspect("/.crosspoint/companion/alongf~1.jso"), CompanionFilePresence::Error);
  state.failShortName = false;
  EXPECT_EQ(lookup.inspect("/.crosspoint/companion/alongf~1.jso"), CompanionFilePresence::Missing);
}
TEST_F(CompanionFileLookupTest, InvalidTargetsDoNotBecomeMissing) {
  for (const char* target :
       {static_cast<const char*>(nullptr), "/", "/.crosspoint/companion", "/.crosspoint/companion/", "/elsewhere/file",
        "/.crosspoint/companion/sub/file", "/.crosspoint/companion/../file", "/.crosspoint/companion/\xc0\xaf"})
    EXPECT_EQ(lookup.inspect(target), CompanionFilePresence::Error);
}
TEST_F(CompanionFileLookupTest, UnicodeCaseAliasesAndSupplementaryNamesAreRecognized) {
  auto& state = inventory_hal_test::state;
  state.files["/.crosspoint/companion/café.json"] = {1};
  state.files["/.crosspoint/companion/📖-book.json"] = {2};
  EXPECT_EQ(lookup.inspect("/.crosspoint/companion/CAFÉ.JSON"), CompanionFilePresence::Present);
  EXPECT_EQ(lookup.inspect("/.crosspoint/companion/📖-BOOK.JSON"), CompanionFilePresence::Present);
  EXPECT_EQ(lookup.inspect("/.crosspoint/companion/cafe\xcc\x81.json"), CompanionFilePresence::Missing);
  EXPECT_EQ(lookup.inspect("/.crosspoint/companion/other-é.json"), CompanionFilePresence::Missing);
}
TEST_F(CompanionFileLookupTest, Utf8FilenameBeyondOldBufferLimitIsRecognized) {
  std::string lower = "/.crosspoint/companion/", upper = lower;
  for (unsigned at = 0; at < 130; ++at) {
    lower += "é";
    upper += "É";
  }
  lower += ".json";
  upper += ".JSON";
  inventory_hal_test::state.files[lower] = {1};
  EXPECT_EQ(lookup.inspect(upper.c_str()), CompanionFilePresence::Present);
}
TEST_F(CompanionFileLookupTest, MalformedDirectoryNameCannotProveAbsence) {
  inventory_hal_test::state.files["/.crosspoint/companion/foreign-\xed\xa0\x80"] = {1};
  EXPECT_EQ(lookup.inspect("/.crosspoint/companion/missing"), CompanionFilePresence::Error);
}
TEST_F(CompanionFileLookupTest, CancellationAndLargeScansYieldAndAllowRetry) {
  auto& state = inventory_hal_test::state;
  for (unsigned at = 0; at < 100; ++at) state.files["/.crosspoint/companion/file" + std::to_string(at)] = {};
  bool allowed = false;
  HalCompanionFileLookup cancelled([](void* context) { return *static_cast<bool*>(context); }, &allowed);
  EXPECT_EQ(cancelled.inspect("/.crosspoint/companion/missing"), CompanionFilePresence::Error);
  allowed = true;
  EXPECT_EQ(cancelled.inspect("/.crosspoint/companion/missing"), CompanionFilePresence::Missing);
  EXPECT_GE(state.yields, 3u);
}
}  // namespace
