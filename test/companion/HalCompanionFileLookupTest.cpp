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
  state.files["/.crosspoint/companion/" + std::string(256, 'a')] = {};
  EXPECT_EQ(lookup.inspect(TARGET), CompanionFilePresence::Error);
  state.files.clear();
  state.directories.erase(TRANSFER_DIRECTORY);
  EXPECT_EQ(lookup.inspect(TARGET), CompanionFilePresence::Error);
}
TEST_F(CompanionFileLookupTest, InvalidTargetsDoNotBecomeMissing) {
  for (const char* target :
       {static_cast<const char*>(nullptr), "/", "/.crosspoint/companion", "/.crosspoint/companion/", "/elsewhere/file",
        "/.crosspoint/companion/sub/file", "/.crosspoint/companion/../file", "/.crosspoint/companion/é"})
    EXPECT_EQ(lookup.inspect(target), CompanionFilePresence::Error);
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
