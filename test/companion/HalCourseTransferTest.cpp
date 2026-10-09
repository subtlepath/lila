#include <Memory.h>
#include <gtest/gtest.h>
#include <openssl/sha.h>

#include <fstream>
#include <iterator>

#include "HalStorage.h"
#include "lib/Companion/CompanionCourseBinding.h"
#include "lib/Companion/CompanionCourseSwitchHandler.h"
#include "lib/Companion/CompanionCourseSwitchIntent.h"
#include "lib/Companion/CompanionDictionaryInstallationTemporaries.h"
#include "lib/Companion/CompanionDictionaryJournalPaths.h"
#include "lib/Companion/CompanionInventoryHandler.h"
#include "lib/Companion/CompanionLegacyCourseStateMigration.h"
#include "lib/Companion/CompanionStoredCourseContinuity.h"
#include "lib/Companion/CompanionTintaJournalPaths.h"
#include "lib/Companion/CompanionTransferHandler.h"
#include "lib/Companion/CompanionWorkspace.h"
// Arduino GPIO headers define HIGH before the installer in native builds.
#define HIGH 1
#include <HalMemory.h>

#include "lib/hal/HalCompanionDictionaryInstaller.h"
#undef HIGH
#include "lib/hal/HalCourseStateMigration.h"
#include "lib/hal/HalDictionaryBindings.h"
#include "lib/hal/HalDictionaryCacheStorage.h"
#include "lib/hal/HalInventoryBuildSession.h"
#include "lib/hal/HalInventoryCourseResolver.h"
#include "lib/hal/HalInventoryResolverSession.h"
// Arduino Print.h defines HEX before these headers in the firmware build.
#define HEX 16
#include "lib/hal/HalJournalMergeCandidateSession.h"
#include "lib/hal/HalJournalMergeStartupRecovery.h"
#include "lib/hal/HalTintaDerivedStartupRecovery.h"
#include "lib/hal/HalTintaJournalMergeCommitContext.h"
#include "lib/hal/HalTintaMergedJournalReconciliation.h"
#undef HEX
#include "lib/hal/HalCompletedRemovalJournalRelease.h"
#include "lib/hal/HalCourseRemovalMetadata.h"
#include "lib/hal/HalCourseRemovalRecovery.h"
#include "lib/hal/HalTransferStorage.h"
#include "platform/StateFiles.h"
namespace tinta::platform {
void log(const char*, ...) {}
}  // namespace tinta::platform
using namespace companion;

class HalCourseTransferTest : public testing::Test {
 protected:
  std::vector<uint8_t> bytes;
  std::array<uint8_t, SESSION_WORKSPACE_SIZE> scratch{};
  Identity generation{};
  TransferDeclaration declaration;
  HalTransferStorage storage;
  void SetUp() override {
    inventory_hal_test::state = {};
    companion_memory_test::internal = {1024 * 1024, 1024 * 1024, 1024 * 1024, 1024 * 1024};
    std::ifstream input(COURSE_FIXTURE, std::ios::binary);
    bytes = {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    ASSERT_FALSE(bytes.empty());
    generation[0] = 1;
    declaration.state.transaction[0] = 2;
    declaration.state.owner[0] = 3;
    declaration.state.storageGeneration = generation;
    declaration.manifest.kind = ContentKind::Course;
    declaration.manifest.length = bytes.size();
    declaration.manifest.formatVersion = 1;
    declaration.manifest.logicalIdentity[0] = 4;
    hashBytes();
  }
  void hashBytes() {
    SHA256(bytes.data(), bytes.size(), declaration.manifest.contentHash.data());
    declaration.state.contentHash = declaration.manifest.contentHash;
    declaration.state.length = declaration.manifest.length;
  }
  void sealPack() {
    std::fill_n(bytes.begin() + 20, 4, 0);
    const auto crc = inventoryIndexCrc(bytes);
    for (unsigned at = 0; at < 4; ++at) bytes[20 + at] = static_cast<uint8_t>(crc >> (8 * at));
    declaration.manifest.length = bytes.size();
    hashBytes();
  }
  void removeInstalledCourse() {
    auto& hal = inventory_hal_test::state;
    hal.enumerateFileMap = true;
    hal.directories["/"] = {};
    hal.directories["/tinta"] = {};
    hal.files[ACTIVE_COURSE_PATH] = bytes;
    std::array<uint8_t, COURSE_BINDING_SIZE> binding{};
    ASSERT_EQ(encodeCourseBinding(declaration.manifest, binding), binding.size());
    hal.files[COURSE_BINDING_PATH] = {binding.begin(), binding.end()};
    ASSERT_TRUE(prepareMigratedCourseState(storage, declaration.manifest.logicalIdentity, scratch));
    CourseRemovalPlan plan;
    plan.request.manifest = declaration.manifest;
    plan.request.generation = generation;
    plan.request.owner = declaration.state.owner;
    plan.request.transaction.fill(71);
    std::array<uint8_t, COURSE_REMOVAL_PLAN_SIZE> encoded{};
    ASSERT_EQ(CourseRemovalPlanCodec::encode(plan, encoded), encoded.size());
    std::array<uint8_t, REMOVAL_STAGE_CLAIM_SIZE> comparison{};
    HalCourseRemovalPlanStorage plans(comparison);
    ContentRemovalRecord initial;
    initial.request = plan.request;
    ASSERT_EQ(plans.publish(encoded, 9, initial.planHash), CourseRemovalPlanStorageResult::Ok);
    std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> journalBytes{}, receiptBytes{}, releaseBytes{};
    HalContentRemovalJournalStorage journalStorage;
    ContentRemovalJournal journal(journalStorage, journalBytes);
    ASSERT_EQ(journal.begin(initial), ContentRemovalJournalResult::Ok);
    auto permission = [](void*) { return true; };
    HalCourseRemovalMetadata metadata(permission, nullptr);
    HalCourseRemovalRecovery recovery(journal, metadata, scratch, permission, nullptr);
    ASSERT_TRUE(recovery.run(initial));
    HalCompletedContentRemovals completions(receiptBytes);
    ASSERT_EQ(completions.persist(*journal.current(), journal), CompletedRemovalResult::Ok);
    HalCompletedRemovalJournalRelease release(journal, journalStorage, completions, releaseBytes);
    ASSERT_EQ(release.release(), CompletedRemovalResult::Ok);
    ASSERT_FALSE(hal.files.contains(ACTIVE_COURSE_PATH));
  }
  uint32_t addIdentityHistory() {
    tinta::core::pack::Pack pack;
    EXPECT_EQ(pack.open(bytes.data(), bytes.size()), tinta::core::pack::PackStatus::Ok);
    uint32_t count = 0;
    for (uint32_t index = 0; index < pack.itemCount(); ++index) count = std::max(count, pack.uidAt(index));
    ++count;  // Retired UID retained beyond the active catalog.
    tinta::core::pack::Header header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    const auto original = bytes;
    const uint32_t directory = (bytes.size() + 3) & ~3u;
    const uint32_t identity = directory + (header.sectionCount + 1) * sizeof(tinta::core::pack::DirEntry);
    bytes.resize(identity + count * 36, 0);
    std::memcpy(bytes.data() + directory, original.data() + header.directoryOffset,
                header.sectionCount * sizeof(tinta::core::pack::DirEntry));
    tinta::core::pack::DirEntry entry{0x4e454449, identity, count * 36, count};
    std::memcpy(bytes.data() + directory + header.sectionCount * sizeof(entry), &entry, sizeof(entry));
    header.directoryOffset = directory;
    ++header.sectionCount;
    header.size = bytes.size();
    std::memcpy(bytes.data(), &header, sizeof(header));
    for (uint32_t index = 0; index < count; ++index) {
      const uint32_t uid = index + 1;
      std::memcpy(bytes.data() + identity + index * 36, &uid, sizeof(uid));
      bytes[identity + index * 36 + 4] = 1;
    }
    sealPack();
    return identity;
  }
  void receive(Transfer& transfer, bool framed = true, std::string_view path = ACTIVE_COURSE_PATH) {
    ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
    std::array<uint8_t, MAX_CONTROL_PAYLOAD> body{};
    std::array<uint8_t, 1 + TRANSFER_STATE_SIZE> response{};
    if (framed) {
      ASSERT_EQ(encodeTransferDeclaration(declaration, body), TRANSFER_DECLARATION_SIZE);
      body[TRANSFER_DECLARATION_SIZE] = path.size();
      std::copy(path.begin(), path.end(), body.begin() + TRANSFER_DECLARATION_SIZE + 1);
      ASSERT_EQ(handleTransfer(transfer, Command::BeginTransfer,
                               std::span(body).first(TRANSFER_DECLARATION_SIZE + 1 + path.size()),
                               declaration.state.owner, response),
                response.size());
      ASSERT_EQ(response[0], static_cast<uint8_t>(TransferResult::Ok));
    } else {
      ASSERT_EQ(transfer.begin(declaration, path), TransferResult::Ok);
    }
    for (size_t offset = 0; offset < bytes.size();) {
      const auto chunk = std::span(bytes).subspan(offset, std::min(size_t{1000}, bytes.size() - offset));
      if (framed) {
        std::copy(declaration.state.transaction.begin(), declaration.state.transaction.end(), body.begin());
        for (size_t i = 0; i < 8; ++i) body[16 + i] = static_cast<uint8_t>(offset >> (8 * i));
        std::copy(chunk.begin(), chunk.end(), body.begin() + CHUNK_HEADER_SIZE);
        ASSERT_EQ(
            handleTransfer(transfer, Command::TransferChunk, std::span(body).first(CHUNK_HEADER_SIZE + chunk.size()),
                           declaration.state.owner, response),
            response.size());
        ASSERT_EQ(response[0], static_cast<uint8_t>(TransferResult::Ok));
      } else {
        ASSERT_EQ(transfer.append(declaration.state.transaction, declaration.state.owner, offset, chunk),
                  TransferResult::Ok);
      }
      offset += chunk.size();
    }
  }
};
TEST_F(HalCourseTransferTest, InstallsCompatibleIdentityHistoryWithoutChangingLearningState) {
  addIdentityHistory();
  auto& hal = inventory_hal_test::state;
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  hal.files["/tinta/items.bin"] = {17};
  std::array<uint8_t, COURSE_BINDING_SIZE> binding;
  ASSERT_EQ(encodeCourseBinding(declaration.manifest, binding), binding.size());
  hal.files[COURSE_BINDING_PATH] = {binding.begin(), binding.end()};
  bytes[12] ^= 1;  // Build timestamp changes without altering item meaning.
  sealPack();
  Transfer transfer(storage, scratch);
  receive(transfer);
  ASSERT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::Ok);
  EXPECT_EQ(hal.files.at(ACTIVE_COURSE_PATH), bytes);
  EXPECT_EQ(hal.files.at("/tinta/items.bin"), std::vector<uint8_t>({17}));
}
TEST_F(HalCourseTransferTest, LegacyStateMigrationResumesAfterRenameFailureWithoutOverwriting) {
  auto& hal = inventory_hal_test::state;
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  std::array<uint8_t, COURSE_BINDING_SIZE> binding;
  ASSERT_EQ(encodeCourseBinding(declaration.manifest, binding), binding.size());
  hal.files[COURSE_BINDING_PATH] = {binding.begin(), binding.end()};
  hal.files["/tinta/items.bin"] = {17};
  hal.files["/tinta/profile.bin.tmp"] = {23};
  hal.failRename = hal.renames + 3;
  EXPECT_EQ(migrateLegacyCourseState(storage, declaration.manifest.logicalIdentity, scratch),
            CourseStateMigrationResult::IoError);
  EXPECT_FALSE(hal.files.contains("/tinta/items.bin"));
  EXPECT_TRUE(hal.files.contains("/tinta/profile.bin.tmp"));
  hal.failRename = 0;
  HalTransferStorage reopened;
  ASSERT_EQ(migrateLegacyCourseState(reopened, declaration.manifest.logicalIdentity, scratch),
            CourseStateMigrationResult::Ok);
  char destination[COURSE_STATE_PATH_SIZE];
  ASSERT_TRUE(courseStatePath(declaration.manifest.logicalIdentity, "items.bin", destination));
  EXPECT_EQ(hal.files.at(destination), std::vector<uint8_t>({17}));
  ASSERT_TRUE(courseStatePath(declaration.manifest.logicalIdentity, "profile.bin.tmp", destination));
  EXPECT_EQ(hal.files.at(destination), std::vector<uint8_t>({23}));
  EXPECT_FALSE(hal.files.contains("/tinta/profile.bin.tmp"));
  EXPECT_EQ(migrateLegacyCourseState(reopened, declaration.manifest.logicalIdentity, scratch),
            CourseStateMigrationResult::Ok);
  ASSERT_TRUE(courseStatePath(declaration.manifest.logicalIdentity, "items.bin", destination));
  hal.files.erase(destination);
  EXPECT_EQ(migrateLegacyCourseState(reopened, declaration.manifest.logicalIdentity, scratch),
            CourseStateMigrationResult::Ok);
  hal.files[destination] = {17};
  hal.files.erase(COURSE_STATE_MIGRATION_DONE);
  ASSERT_TRUE(courseStatePath(declaration.manifest.logicalIdentity, "profile.bin.tmp", destination));
  hal.files["/tinta/profile.bin.tmp"] = {29};
  EXPECT_EQ(migrateLegacyCourseState(reopened, declaration.manifest.logicalIdentity, scratch),
            CourseStateMigrationResult::Conflict);
  EXPECT_EQ(hal.files.at(destination), std::vector<uint8_t>({23}));
  hal.files.erase("/tinta/profile.bin.tmp");
  ASSERT_TRUE(courseStatePath(declaration.manifest.logicalIdentity, "items.bin", destination));
  hal.files.erase(destination);
  EXPECT_EQ(migrateLegacyCourseState(reopened, declaration.manifest.logicalIdentity, scratch),
            CourseStateMigrationResult::Corrupt);
}
TEST_F(HalCourseTransferTest, LegacyMigrationCompletionSyncFailureCanResume) {
  auto& hal = inventory_hal_test::state;
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  std::array<uint8_t, COURSE_BINDING_SIZE> binding;
  ASSERT_EQ(encodeCourseBinding(declaration.manifest, binding), binding.size());
  hal.files[COURSE_BINDING_PATH] = {binding.begin(), binding.end()};
  hal.files["/tinta/items.bin"] = {17};
  hal.failSyncPath = COURSE_STATE_MIGRATION_DONE_STAGE;
  EXPECT_EQ(migrateLegacyCourseState(storage, declaration.manifest.logicalIdentity, scratch),
            CourseStateMigrationResult::IoError);
  EXPECT_FALSE(hal.files.contains(COURSE_STATE_MIGRATION_DONE));
  EXPECT_FALSE(hal.files.contains("/tinta/items.bin"));
  hal.failSyncPath.clear();
  HalTransferStorage reopened;
  ASSERT_EQ(migrateLegacyCourseState(reopened, declaration.manifest.logicalIdentity, scratch),
            CourseStateMigrationResult::Ok);
  char destination[COURSE_STATE_PATH_SIZE];
  ASSERT_TRUE(courseStatePath(declaration.manifest.logicalIdentity, "items.bin", destination));
  EXPECT_EQ(hal.files.at(destination), std::vector<uint8_t>({17}));
  EXPECT_TRUE(hal.files.contains(COURSE_STATE_MIGRATION_DONE));
  EXPECT_FALSE(hal.files.contains(COURSE_STATE_MIGRATION_DONE_STAGE));
}
TEST_F(HalCourseTransferTest, HalMigrationDirectoryFailureLeavesLegacyStateAvailableForRetry) {
  auto& hal = inventory_hal_test::state;
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  std::array<uint8_t, COURSE_BINDING_SIZE> binding;
  ASSERT_EQ(encodeCourseBinding(declaration.manifest, binding), binding.size());
  hal.files[COURSE_BINDING_PATH] = {binding.begin(), binding.end()};
  hal.files["/tinta/items.bin"] = {17};
  hal.failDirectory = true;
  EXPECT_FALSE(prepareMigratedCourseState(storage, declaration.manifest.logicalIdentity, scratch));
  EXPECT_EQ(hal.files.at("/tinta/items.bin"), std::vector<uint8_t>({17}));
  EXPECT_FALSE(hal.files.contains(COURSE_STATE_MIGRATION));
  hal.failDirectory = false;
  ASSERT_TRUE(prepareMigratedCourseState(storage, declaration.manifest.logicalIdentity, scratch));
  EXPECT_FALSE(hal.files.contains("/tinta/items.bin"));
  EXPECT_TRUE(hal.files.contains(COURSE_STATE_MIGRATION_DONE));
}
TEST_F(HalCourseTransferTest, MigratedStateOpensAndPersistsThroughProductionStateFiles) {
  auto& hal = inventory_hal_test::state;
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  std::array<uint8_t, COURSE_BINDING_SIZE> binding;
  ASSERT_EQ(encodeCourseBinding(declaration.manifest, binding), binding.size());
  hal.files[COURSE_BINDING_PATH] = {binding.begin(), binding.end()};
  tinta::platform::StateFiles state;
  state.begin();
  const uint8_t previous = 17, updated = 23;
  ASSERT_TRUE(state.write("items.bin", 0, &previous, 1));
  state.release();
  ASSERT_TRUE(prepareMigratedCourseState(storage, declaration.manifest.logicalIdentity, scratch));
  ASSERT_TRUE(state.beginCourse(declaration.manifest.logicalIdentity));
  uint8_t value = 0;
  ASSERT_EQ(state.read("items.bin", 0, &value, 1), 1);
  EXPECT_EQ(value, previous);
  ASSERT_TRUE(state.replace("items.bin", &updated, 1));
  ASSERT_TRUE(state.remove("session.bin"));
  state.release();
  HalTransferStorage reopenedStorage;
  ASSERT_TRUE(prepareMigratedCourseState(reopenedStorage, declaration.manifest.logicalIdentity, scratch));
  tinta::platform::StateFiles reopened;
  ASSERT_TRUE(reopened.beginCourse(declaration.manifest.logicalIdentity));
  ASSERT_EQ(reopened.read("items.bin", 0, &value, 1), 1);
  EXPECT_EQ(value, updated);
  EXPECT_EQ(reopened.size("session.bin"), -1);
  EXPECT_FALSE(hal.files.contains("/tinta/items.bin"));
}
TEST_F(HalCourseTransferTest, CompletedPrimaryMigrationStillRecoversLegacyStarsAndReadings) {
  auto& hal = inventory_hal_test::state;
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  std::array<uint8_t, COURSE_BINDING_SIZE> binding;
  ASSERT_EQ(encodeCourseBinding(declaration.manifest, binding), binding.size());
  hal.files[COURSE_BINDING_PATH] = {binding.begin(), binding.end()};
  ASSERT_EQ(migrateLegacyCourseState(storage, declaration.manifest.logicalIdentity, scratch),
            CourseStateMigrationResult::Ok);
  hal.files["/tinta/starred.bin"] = {17};
  hal.files["/tinta/read.bin.tmp"] = {23};
  hal.failRename = hal.renames + 3;
  EXPECT_FALSE(prepareMigratedCourseState(storage, declaration.manifest.logicalIdentity, scratch));
  EXPECT_FALSE(hal.files.contains("/tinta/starred.bin"));
  EXPECT_TRUE(hal.files.contains("/tinta/read.bin.tmp"));
  hal.failRename = 0;
  ASSERT_TRUE(prepareMigratedCourseState(storage, declaration.manifest.logicalIdentity, scratch));
  char destination[COURSE_STATE_PATH_SIZE];
  ASSERT_TRUE(courseStatePath(declaration.manifest.logicalIdentity, "starred.bin", destination));
  EXPECT_EQ(hal.files.at(destination), std::vector<uint8_t>({17}));
  ASSERT_TRUE(courseStatePath(declaration.manifest.logicalIdentity, "read.bin.tmp", destination));
  EXPECT_EQ(hal.files.at(destination), std::vector<uint8_t>({23}));
  EXPECT_FALSE(hal.files.contains("/tinta/read.bin.tmp"));
  EXPECT_TRUE(hal.files.contains(COURSE_MARK_MIGRATION_PATHS.done));
}
TEST_F(HalCourseTransferTest, MarkMigrationRecoversEveryRenameBeforeAndAfterItsEffect) {
  static constexpr const char* NAMES[] = {"starred.bin", "starred.bin.tmp", "read.bin", "read.bin.tmp"};
  for (bool afterEffect : {false, true}) {
    for (unsigned step = 1; step <= 6; ++step) {
      SCOPED_TRACE(afterEffect);
      SCOPED_TRACE(step);
      auto& hal = inventory_hal_test::state;
      hal = {};
      HalTransferStorage actual;
      hal.files[ACTIVE_COURSE_PATH] = bytes;
      std::array<uint8_t, COURSE_BINDING_SIZE> binding;
      ASSERT_EQ(encodeCourseBinding(declaration.manifest, binding), binding.size());
      hal.files[COURSE_BINDING_PATH] = {binding.begin(), binding.end()};
      ASSERT_EQ(migrateLegacyCourseState(actual, declaration.manifest.logicalIdentity, scratch),
                CourseStateMigrationResult::Ok);
      for (unsigned at = 0; at < std::size(NAMES); ++at) {
        hal.files[std::string("/tinta/") + NAMES[at]] = {static_cast<uint8_t>(17 + at)};
      }
      if (afterEffect)
        hal.failRenameAfter = hal.renames + step;
      else
        hal.failRename = hal.renames + step;
      EXPECT_FALSE(prepareMigratedCourseState(actual, declaration.manifest.logicalIdentity, scratch));
      hal.failRename = hal.failRenameAfter = 0;
      HalTransferStorage recovered;
      ASSERT_TRUE(prepareMigratedCourseState(recovered, declaration.manifest.logicalIdentity, scratch));
      for (unsigned at = 0; at < std::size(NAMES); ++at) {
        char destination[COURSE_STATE_PATH_SIZE];
        ASSERT_TRUE(courseStatePath(declaration.manifest.logicalIdentity, NAMES[at], destination));
        EXPECT_EQ(hal.files.at(destination), std::vector<uint8_t>({static_cast<uint8_t>(17 + at)}));
        EXPECT_FALSE(hal.files.contains(std::string("/tinta/") + NAMES[at]));
      }
      const auto original = hal.files;
      ASSERT_TRUE(prepareMigratedCourseState(recovered, declaration.manifest.logicalIdentity, scratch));
      EXPECT_EQ(hal.files, original);
    }
  }
}
TEST_F(HalCourseTransferTest, MarkMigrationRecoversFailedIntentAndCompletionSync) {
  for (const auto* stage : {COURSE_MARK_MIGRATION_PATHS.stage, COURSE_MARK_MIGRATION_PATHS.doneStage}) {
    auto& hal = inventory_hal_test::state;
    hal = {};
    HalTransferStorage actual;
    hal.files[ACTIVE_COURSE_PATH] = bytes;
    std::array<uint8_t, COURSE_BINDING_SIZE> binding;
    ASSERT_EQ(encodeCourseBinding(declaration.manifest, binding), binding.size());
    hal.files[COURSE_BINDING_PATH] = {binding.begin(), binding.end()};
    ASSERT_EQ(migrateLegacyCourseState(actual, declaration.manifest.logicalIdentity, scratch),
              CourseStateMigrationResult::Ok);
    hal.files["/tinta/starred.bin"] = {17};
    hal.failSyncPath = stage;
    EXPECT_FALSE(prepareMigratedCourseState(actual, declaration.manifest.logicalIdentity, scratch));
    EXPECT_FALSE(hal.files.contains(COURSE_MARK_MIGRATION_PATHS.done));
    if (stage == COURSE_MARK_MIGRATION_PATHS.stage) {
      EXPECT_TRUE(hal.files.contains("/tinta/starred.bin"));
    }
    hal.failSyncPath.clear();
    HalTransferStorage recovered;
    ASSERT_TRUE(prepareMigratedCourseState(recovered, declaration.manifest.logicalIdentity, scratch));
    char destination[COURSE_STATE_PATH_SIZE];
    ASSERT_TRUE(courseStatePath(declaration.manifest.logicalIdentity, "starred.bin", destination));
    EXPECT_EQ(hal.files.at(destination), std::vector<uint8_t>({17}));
  }
}
TEST_F(HalCourseTransferTest, ActiveCourseSelectionMigratesBeforeExposingNamespace) {
  auto& hal = inventory_hal_test::state;
  Identity selected{};
  bool bound = true;
  ASSERT_TRUE(selectActiveCourseState(storage, scratch, selected, bound));
  EXPECT_FALSE(bound);
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  std::array<uint8_t, COURSE_BINDING_SIZE> binding;
  ASSERT_EQ(encodeCourseBinding(declaration.manifest, binding), binding.size());
  hal.files[COURSE_BINDING_PATH] = {binding.begin(), binding.end()};
  hal.files["/tinta/items.bin"] = {17};
  ASSERT_TRUE(selectActiveCourseState(storage, scratch, selected, bound));
  EXPECT_TRUE(bound);
  EXPECT_EQ(selected, declaration.manifest.logicalIdentity);
  EXPECT_FALSE(hal.files.contains("/tinta/items.bin"));
  EXPECT_TRUE(hal.files.contains(COURSE_STATE_MIGRATION_DONE));
  static constexpr char NAME[] = "Luis";
  auto name = std::search(bytes.begin(), bytes.end(), NAME, NAME + 4);
  ASSERT_NE(name, bytes.end());
  name[3] = 'z';
  sealPack();
  hal.files["/candidate.pack"] = bytes;
  EXPECT_FALSE(storage.validateContent(ACTIVE_COURSE_PATH, "/candidate.pack", declaration.manifest, scratch));
  hal.files.erase(COURSE_BINDING_PATH);
  EXPECT_FALSE(selectActiveCourseState(storage, scratch, selected, bound));
}
TEST_F(HalCourseTransferTest, ActiveCourseSelectionRejectsUnboundRecoveryRecordsWithoutChangingSelection) {
  auto& hal = inventory_hal_test::state;
  Identity selected = declaration.manifest.logicalIdentity;
  bool bound = true;
  for (const auto* path :
       {COURSE_STATE_MIGRATION, COURSE_STATE_MIGRATION_STAGE, COURSE_STATE_MIGRATION_DONE,
        COURSE_STATE_MIGRATION_DONE_STAGE, COURSE_MARK_MIGRATION_PATHS.intent, COURSE_MARK_MIGRATION_PATHS.stage,
        COURSE_MARK_MIGRATION_PATHS.done, COURSE_MARK_MIGRATION_PATHS.doneStage}) {
    hal.files[path] = {1, 2, 3};
    const auto original = hal.files;
    EXPECT_FALSE(selectActiveCourseState(storage, scratch, selected, bound));
    EXPECT_EQ(selected, declaration.manifest.logicalIdentity);
    EXPECT_TRUE(bound);
    EXPECT_EQ(hal.files, original);
    hal.files.erase(path);
  }
  hal.files[COURSE_BINDING_PATH] = {1, 2, 3};
  EXPECT_FALSE(selectActiveCourseState(storage, scratch, selected, bound));
  EXPECT_EQ(selected, declaration.manifest.logicalIdentity);
  EXPECT_TRUE(bound);
}
TEST_F(HalCourseTransferTest, LegacyMigrationRefusesExistingDestinationWithoutWritingIntent) {
  auto& hal = inventory_hal_test::state;
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  std::array<uint8_t, COURSE_BINDING_SIZE> binding;
  ASSERT_EQ(encodeCourseBinding(declaration.manifest, binding), binding.size());
  hal.files[COURSE_BINDING_PATH] = {binding.begin(), binding.end()};
  hal.files["/tinta/items.bin"] = {17};
  char destination[COURSE_STATE_PATH_SIZE];
  ASSERT_TRUE(courseStatePath(declaration.manifest.logicalIdentity, "items.bin", destination));
  hal.files[destination] = {23};
  const auto original = hal.files;
  EXPECT_EQ(migrateLegacyCourseState(storage, declaration.manifest.logicalIdentity, scratch),
            CourseStateMigrationResult::Conflict);
  EXPECT_EQ(hal.files, original);
  EXPECT_EQ(hal.renames, 0u);
}
TEST_F(HalCourseTransferTest, LegacyMigrationIntentSyncFailureLeavesEverySourceInPlace) {
  auto& hal = inventory_hal_test::state;
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  std::array<uint8_t, COURSE_BINDING_SIZE> binding;
  ASSERT_EQ(encodeCourseBinding(declaration.manifest, binding), binding.size());
  hal.files[COURSE_BINDING_PATH] = {binding.begin(), binding.end()};
  hal.files["/tinta/items.bin"] = {17};
  hal.failSyncPath = COURSE_STATE_MIGRATION_STAGE;
  EXPECT_EQ(migrateLegacyCourseState(storage, declaration.manifest.logicalIdentity, scratch),
            CourseStateMigrationResult::IoError);
  EXPECT_EQ(hal.files.at("/tinta/items.bin"), std::vector<uint8_t>({17}));
  EXPECT_FALSE(hal.files.contains(COURSE_STATE_MIGRATION));
  EXPECT_EQ(hal.renames, 0u);
  hal.failSyncPath.clear();
  ASSERT_EQ(migrateLegacyCourseState(storage, declaration.manifest.logicalIdentity, scratch),
            CourseStateMigrationResult::Ok);
  EXPECT_FALSE(hal.files.contains("/tinta/items.bin"));
}
TEST_F(HalCourseTransferTest, LegacyMigrationRejectsCorruptAndForeignRecordsWithoutMutation) {
  auto& hal = inventory_hal_test::state;
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  std::array<uint8_t, COURSE_BINDING_SIZE> binding;
  ASSERT_EQ(encodeCourseBinding(declaration.manifest, binding), binding.size());
  hal.files[COURSE_BINDING_PATH] = {binding.begin(), binding.end()};
  hal.files["/tinta/items.bin"] = {17};
  ASSERT_EQ(migrateLegacyCourseState(storage, declaration.manifest.logicalIdentity, scratch),
            CourseStateMigrationResult::Ok);
  const auto committed = hal.files;
  for (const auto* path : {COURSE_STATE_MIGRATION, COURSE_STATE_MIGRATION_DONE}) {
    SCOPED_TRACE(path);
    hal.files = committed;
    hal.files.at(path)[0] ^= 1;
    const auto corrupted = hal.files;
    EXPECT_EQ(migrateLegacyCourseState(storage, declaration.manifest.logicalIdentity, scratch),
              CourseStateMigrationResult::Corrupt);
    EXPECT_EQ(hal.files, corrupted);
  }
  hal.files = committed;
  auto& intent = hal.files.at(COURSE_STATE_MIGRATION);
  intent[4] ^= 1;
  const auto crc = courseBindingCrc(std::span(intent).first(24));
  for (unsigned at = 0; at < 4; ++at) intent[24 + at] = static_cast<uint8_t>(crc >> (8 * at));
  const auto foreign = hal.files;
  EXPECT_EQ(migrateLegacyCourseState(storage, declaration.manifest.logicalIdentity, scratch),
            CourseStateMigrationResult::Conflict);
  EXPECT_EQ(hal.files, foreign);
}
TEST_F(HalCourseTransferTest, StoredIdentityComparisonYieldsDuringLongScans) {
  const auto legacy = bytes;
  addIdentityHistory();
  auto& files = inventory_hal_test::state.files;
  files["/current.pack"] = bytes;
  files["/candidate.pack"] = bytes;
  static unsigned yields = 0;
  yields = 0;
  const auto yield = [] { ++yields; };
  EXPECT_EQ(compareStoredCourseItemIdentities(storage, "/current.pack", "/candidate.pack", yield),
            CourseItemContinuity::Compatible);
  EXPECT_GT(yields, 0u);
  const auto uncachedReads = inventory_hal_test::state.reads;
  EXPECT_EQ(compareStoredCourseItemIdentities(storage, "/current.pack", "/candidate.pack", yield, scratch),
            CourseItemContinuity::Compatible);
  EXPECT_LT(inventory_hal_test::state.reads - uncachedReads, uncachedReads);
  yields = 0;
  files["/current.pack"] = legacy;
  EXPECT_TRUE(sameLegacyCourseRecords(storage, "/current.pack", "/candidate.pack", scratch, yield));
  EXPECT_GT(yields, 0u);
}
TEST_F(HalCourseTransferTest, AddsIdentityBaselineToLegacyCourseWithoutChangingLearningContent) {
  const auto original = bytes;
  std::array<uint8_t, COURSE_BINDING_SIZE> binding;
  ASSERT_EQ(encodeCourseBinding(declaration.manifest, binding), binding.size());
  addIdentityHistory();
  auto& hal = inventory_hal_test::state;
  hal.files[ACTIVE_COURSE_PATH] = original;
  hal.files[COURSE_BINDING_PATH] = {binding.begin(), binding.end()};
  hal.files["/tinta/items.bin"] = {17};
  Transfer transfer(storage, scratch);
  receive(transfer);
  ASSERT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::Ok);
  EXPECT_EQ(hal.files.at(ACTIVE_COURSE_PATH), bytes);
  EXPECT_EQ(hal.files.at("/tinta/items.bin"), std::vector<uint8_t>({17}));
  ContentManifest installed;
  ASSERT_TRUE(decodeCourseBinding(hal.files.at(COURSE_BINDING_PATH), installed));
  EXPECT_EQ(installed, declaration.manifest);
  const auto baseline = bytes;
  static constexpr char TRANSLATION[] = "house";
  auto text = std::search(bytes.begin(), bytes.end(), TRANSLATION, TRANSLATION + 5);
  ASSERT_NE(text, bytes.end());
  text[4] = 'E';
  declaration.state.transaction[0] = 5;
  sealPack();
  ASSERT_NE(bytes, baseline);
  HalTransferStorage reopenedStorage;
  Transfer edition(reopenedStorage, scratch);
  receive(edition);
  ASSERT_EQ(edition.commit(declaration.state.transaction, declaration.state.owner), TransferResult::Ok);
  EXPECT_EQ(hal.files.at(ACTIVE_COURSE_PATH), bytes);
  EXPECT_EQ(hal.files.at("/tinta/items.bin"), std::vector<uint8_t>({17}));
  ASSERT_TRUE(decodeCourseBinding(hal.files.at(COURSE_BINDING_PATH), installed));
  EXPECT_EQ(installed, declaration.manifest);
  EXPECT_FALSE(hal.files.contains(TRANSFER_BACKUP));
}
TEST_F(HalCourseTransferTest, RecoversCompatibleIdentityUpdateAcrossBothPackRenameFailures) {
  addIdentityHistory();
  const auto original = bytes;
  std::array<uint8_t, COURSE_BINDING_SIZE> binding;
  ASSERT_EQ(encodeCourseBinding(declaration.manifest, binding), binding.size());
  bytes[12] ^= 1;
  sealPack();
  for (unsigned failedRename = 1; failedRename <= 2; ++failedRename) {
    SCOPED_TRACE(failedRename);
    auto& hal = inventory_hal_test::state;
    hal = {};
    hal.files[ACTIVE_COURSE_PATH] = original;
    hal.files[COURSE_BINDING_PATH] = {binding.begin(), binding.end()};
    hal.files["/tinta/items.bin"] = {17};
    {
      Transfer transfer(storage, scratch);
      receive(transfer);
      hal.failRename = hal.renames + failedRename;
      ASSERT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::IoError);
      ASSERT_EQ(transfer.current()->phase, TransferPhase::Installing);
    }
    EXPECT_EQ(hal.files.at("/tinta/items.bin"), std::vector<uint8_t>({17}));
    if (failedRename == 1)
      EXPECT_EQ(hal.files.at(ACTIVE_COURSE_PATH), original);
    else
      EXPECT_EQ(hal.files.at(TRANSFER_BACKUP), original);
    hal.failRename = 0;
    HalTransferStorage rebootStorage;
    Transfer reboot(rebootStorage, scratch);
    ASSERT_EQ(reboot.recover(generation), TransferResult::Ok);
    ASSERT_EQ(reboot.current()->phase, TransferPhase::Committed);
    EXPECT_EQ(hal.files.at(ACTIVE_COURSE_PATH), bytes);
    EXPECT_EQ(hal.files.at("/tinta/items.bin"), std::vector<uint8_t>({17}));
    EXPECT_FALSE(hal.files.contains(TRANSFER_BACKUP));
    ContentManifest installed;
    ASSERT_TRUE(decodeCourseBinding(hal.files.at(COURSE_BINDING_PATH), installed));
    EXPECT_EQ(installed, declaration.manifest);
    EXPECT_EQ(reboot.commit(declaration.state.transaction, declaration.state.owner), TransferResult::Ok);
  }
}
TEST_F(HalCourseTransferTest, RefusesReassignedRetiredIdentityBeforeAnyRename) {
  addIdentityHistory();
  auto& hal = inventory_hal_test::state;
  const auto original = bytes;
  hal.files[ACTIVE_COURSE_PATH] = original;
  hal.files["/tinta/items.bin"] = {17};
  std::array<uint8_t, COURSE_BINDING_SIZE> binding;
  ASSERT_EQ(encodeCourseBinding(declaration.manifest, binding), binding.size());
  hal.files[COURSE_BINDING_PATH] = {binding.begin(), binding.end()};
  bytes.back() = 2;
  sealPack();
  Transfer transfer(storage, scratch);
  receive(transfer);
  EXPECT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::Invalid);
  EXPECT_EQ(hal.files.at(ACTIVE_COURSE_PATH), original);
  EXPECT_EQ(hal.files.at("/tinta/items.bin"), std::vector<uint8_t>({17}));
  EXPECT_EQ(hal.files.at(COURSE_BINDING_PATH), std::vector<uint8_t>(binding.begin(), binding.end()));
  EXPECT_EQ(hal.renames, 0u);
}
TEST_F(HalCourseTransferTest, StoredContinuityDistinguishesMissingFilesFromLegacyHistory) {
  auto& files = inventory_hal_test::state.files;
  files["/current.pack"] = bytes;
  EXPECT_EQ(compareStoredCourseItemIdentities(storage, "/current.pack", "/candidate.pack"),
            CourseItemContinuity::InvalidHistory);
  files["/candidate.pack"] = bytes;
  EXPECT_EQ(compareStoredCourseItemIdentities(storage, "/current.pack", "/candidate.pack"),
            CourseItemContinuity::MissingHistory);
  EXPECT_EQ(files.at("/current.pack"), bytes);
  EXPECT_EQ(files.at("/candidate.pack"), bytes);
  EXPECT_EQ(compareStoredCourseItemIdentities(storage, nullptr, "/candidate.pack"),
            CourseItemContinuity::InvalidHistory);
  inventory_hal_test::state.failRead = inventory_hal_test::state.reads + 1;
  EXPECT_EQ(compareStoredCourseItemIdentities(storage, "/current.pack", "/candidate.pack"),
            CourseItemContinuity::InvalidHistory);
  inventory_hal_test::state.failRead = 0;
  EXPECT_EQ(compareStoredCourseItemIdentities(storage, "/current.pack", "/candidate.pack"),
            CourseItemContinuity::MissingHistory);
}
TEST_F(HalCourseTransferTest, JournalOnlyHistoryProtectsLegacyCourseReplacement) {
  const auto original = bytes;
  std::array<uint8_t, COURSE_BINDING_SIZE> binding;
  ASSERT_EQ(encodeCourseBinding(declaration.manifest, binding), binding.size());
  static constexpr char NAME[] = "Luis";
  auto name = std::search(bytes.begin(), bytes.end(), NAME, NAME + 4);
  ASSERT_NE(name, bytes.end());
  name[3] = 'z';
  sealPack();
  for (const auto* path : {TINTA_JOURNAL_EVENTS, TINTA_JOURNAL_HEADER_A, TINTA_JOURNAL_HEADER_B}) {
    SCOPED_TRACE(path);
    auto& hal = inventory_hal_test::state;
    hal = {};
    hal.files[ACTIVE_COURSE_PATH] = original;
    hal.files[COURSE_BINDING_PATH] = {binding.begin(), binding.end()};
    hal.files["/candidate.pack"] = bytes;
    hal.files[path] = {17};
    EXPECT_FALSE(storage.validateContent(ACTIVE_COURSE_PATH, "/candidate.pack", declaration.manifest, scratch));
    EXPECT_EQ(hal.files.at(ACTIVE_COURSE_PATH), original);
    EXPECT_EQ(hal.files.at(path), std::vector<uint8_t>({17}));
    EXPECT_EQ(hal.renames, 0u);
  }
}
TEST_F(HalCourseTransferTest, ComposedInventoryIncludesBoundCourseAndCompressedDictionaryAndPreservesFailedRescan) {
  auto& hal = inventory_hal_test::state;
  const auto fixture = [](const char* name) {
    std::ifstream file(std::string(COMPANION_FIXTURE_DIR) + name, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
  };
  hal.directories["/"] = {
      {"tinta", true}, {"books", true}, {"fonts", true}, {"dictionaries", true}, {".crosspoint", true}};
  hal.directories["/tinta"] = {{"course.pack", false}, {"items.bin", false}};
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  hal.files["/tinta/items.bin"] = {17};
  std::array<uint8_t, COURSE_BINDING_SIZE> binding{};
  ASSERT_EQ(encodeCourseBinding(declaration.manifest, binding), binding.size());
  hal.files[COURSE_BINDING_PATH] = {binding.begin(), binding.end()};
  hal.directories["/books"] = {{"book.epub", false}};
  hal.files["/books/book.epub"] = {1, 2, 3};
  hal.directories["/fonts"] = {{"font.cpfont", false}};
  hal.files["/fonts/font.cpfont"] = fixture("BitmapFont-v4.fixture");
  ASSERT_FALSE(hal.files["/fonts/font.cpfont"].empty());
  hal.directories["/dictionaries"] = {{"es", true}};
  hal.directories["/dictionaries/es"] = {
      {"stem.dict.dz", false}, {"stem.idx", false}, {"stem.ifo", false}, {"stem.syn", false}};
  hal.files["/dictionaries/es/stem.dict.dz"] = fixture("dictzip-chunks.dict.dz");
  hal.files["/dictionaries/es/stem.idx"] = fixture("DictionaryIndex-definitions.fixture");
  hal.files["/dictionaries/es/stem.syn"] = fixture("DictionaryIndex-synonyms.fixture");
  const std::string info =
      "StarDict's dict ifo file\nversion=3.0.0\nbookname=Canonical "
      "dictionary\nwordcount=2\nidxfilesize=24\nsynwordcount=1\n";
  hal.files["/dictionaries/es/stem.ifo"] = {info.begin(), info.end()};
  auto decoder = makeUniqueNoThrow<tinfl_decompressor>();
  auto window = makeUniqueNoThrow<uint8_t[]>(32768);
  ASSERT_TRUE(decoder);
  ASSERT_TRUE(window);
  auto resolver = makeUniqueNoThrow<HalInventoryResolverSession>(storage, std::span(scratch), *decoder,
                                                                 std::span(window.get(), 32768));
  ASSERT_TRUE(resolver);
  ASSERT_TRUE(resolver->prepareAfterRecovery());
  auto session = makeUniqueNoThrow<HalInventoryBuildSession>(storage, *resolver, std::span(scratch));
  ASSERT_TRUE(session);
  uint64_t revision = 0;
  ASSERT_EQ(session->build(generation, revision), InventoryPublicationResult::Ok);
  HalInventoryIndexStorage reader;
  ASSERT_TRUE(reader.open(InventoryPublication::INDEX));
  std::fill(scratch.begin(), scratch.begin() + TRANSFER_OFFSET, 0xa5);
  IndexedInventoryCatalog catalog(reader, std::span(scratch).subspan(TRANSFER_OFFSET));
  ASSERT_TRUE(catalog.open(generation));
  ASSERT_EQ(catalog.count(), 4u);
  InventoryRequest request;
  request.storageGeneration = generation;
  std::array<uint8_t, INVENTORY_REQUEST_SIZE> body{};
  std::array<uint8_t, MAX_INVENTORY_PAGE_SIZE + 1> response{};
  ASSERT_EQ(encodeInventoryRequest(request, body), body.size());
  EXPECT_EQ(handleInventory(catalog, false, body, response), 1u);
  const auto reply = handleInventory(catalog, true, body, response);
  ASSERT_GT(reply, 1u);
  InventoryPageView page;
  ASSERT_TRUE(decodeInventoryPage(std::span(response).subspan(1, reply - 1), page));
  EXPECT_EQ(page.count(), 4u);
  EXPECT_TRUE(page.header.complete);
  EXPECT_TRUE(
      std::all_of(scratch.begin(), scratch.begin() + TRANSFER_OFFSET, [](uint8_t byte) { return byte == 0xa5; }));
  unsigned kinds = 0;
  for (unsigned at = 0; at < 4; ++at) {
    ContentManifest manifest;
    ASSERT_TRUE(catalog.read(at, manifest));
    kinds |= 1u << static_cast<unsigned>(manifest.kind);
    if (manifest.kind == ContentKind::Course) {
      EXPECT_EQ(manifest, declaration.manifest);
    }
  }
  EXPECT_EQ(kinds, (1u << static_cast<unsigned>(ContentKind::Epub)) |
                       (1u << static_cast<unsigned>(ContentKind::Course)) |
                       (1u << static_cast<unsigned>(ContentKind::Font)) |
                       (1u << static_cast<unsigned>(ContentKind::Dictionary)));
  ASSERT_TRUE(reader.close());
  const auto previousIndex = hal.files.at(InventoryPublication::INDEX);
  const auto previousPaths = hal.files.at(InventoryPublication::PATHS);
  hal.files.at("/dictionaries/es/stem.dict.dz").back() ^= 1;
  EXPECT_EQ(session->build(generation, revision), InventoryPublicationResult::IoError);
  EXPECT_EQ(revision, 1u);
  EXPECT_EQ(hal.files.at(InventoryPublication::INDEX), previousIndex);
  EXPECT_EQ(hal.files.at(InventoryPublication::PATHS), previousPaths);
  EXPECT_EQ(hal.files.at("/tinta/items.bin"), std::vector<uint8_t>({17}));
}
TEST_F(HalCourseTransferTest, InstallsVerifiedPackAndBindingThroughActualHal) {
  Transfer transfer(storage, scratch);
  receive(transfer);
  std::array<uint8_t, 1 + TRANSFER_STATE_SIZE> response{};
  ASSERT_EQ(handleTransfer(transfer, Command::Commit, declaration.state.transaction, declaration.state.owner, response),
            response.size());
  ASSERT_EQ(response[0], static_cast<uint8_t>(TransferResult::Ok));
  EXPECT_EQ(inventory_hal_test::state.files.at(ACTIVE_COURSE_PATH), bytes);
  ContentManifest binding;
  ASSERT_TRUE(decodeCourseBinding(inventory_hal_test::state.files.at(COURSE_BINDING_PATH), binding));
  EXPECT_EQ(binding, declaration.manifest);
  Transfer reboot(storage, scratch);
  ASSERT_EQ(reboot.recover(generation), TransferResult::Ok);
  EXPECT_EQ(reboot.current()->phase, TransferPhase::Committed);
  EXPECT_FALSE(inventory_hal_test::state.files.contains(TRANSFER_BACKUP));
}
TEST_F(HalCourseTransferTest, PendingDictionaryProofBlocksCourseAdmissionWithCheckedLookup) {
  Transfer transfer(storage, scratch);
  ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
  auto& hal = inventory_hal_test::state;
  hal.files[DICTIONARY_RETIREMENT_JOURNALS[1]] = {1};
  hal.falseExists = true;
  const auto saved = hal.files;
  EXPECT_EQ(transfer.begin(declaration, ACTIVE_COURSE_PATH), TransferResult::Busy);
  EXPECT_EQ(hal.files, saved);
  hal.files.erase(DICTIONARY_RETIREMENT_JOURNALS[1]);
  hal.directoryErrorPath = TRANSFER_DIRECTORY;
  const auto beforeError = hal.files;
  EXPECT_EQ(transfer.begin(declaration, ACTIVE_COURSE_PATH), TransferResult::IoError);
  EXPECT_EQ(hal.files, beforeError);
  hal.directoryErrorPath.clear();
  hal.falseExists = false;
  ASSERT_EQ(transfer.begin(declaration, ACTIVE_COURSE_PATH), TransferResult::Ok);
}
TEST_F(HalCourseTransferTest, RefusesUnboundLearnerStateBeforeReplacingPack) {
  inventory_hal_test::state.files["/tinta/items.bin"] = {99};
  Transfer transfer(storage, scratch);
  receive(transfer);
  EXPECT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::Invalid);
  EXPECT_FALSE(inventory_hal_test::state.files.contains(ACTIVE_COURSE_PATH));
  EXPECT_EQ(inventory_hal_test::state.files.at("/tinta/items.bin"), std::vector<uint8_t>({99}));
  EXPECT_EQ(transfer.current()->phase, TransferPhase::Receiving);
}
TEST_F(HalCourseTransferTest, RefusesHashValidButCorruptPackBeforeRenamingOriginal) {
  inventory_hal_test::state.files[ACTIVE_COURSE_PATH] = bytes;
  bytes.back() ^= 1;
  hashBytes();
  const auto original = inventory_hal_test::state.files.at(ACTIVE_COURSE_PATH);
  Transfer transfer(storage, scratch);
  receive(transfer);
  EXPECT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::Invalid);
  EXPECT_EQ(inventory_hal_test::state.files.at(ACTIVE_COURSE_PATH), original);
  EXPECT_FALSE(inventory_hal_test::state.files.contains(TRANSFER_BACKUP));
}
TEST_F(HalCourseTransferTest, RefusesChecksummedMalformedLocaleWithoutReplacingCourseOrProgress) {
  auto& hal = inventory_hal_test::state;
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  hal.files["/tinta/items.bin"] = {17};
  const auto original = bytes;
  std::fill_n(bytes.begin() + 24, 8, 0);
  std::copy_n("es MX", 5, bytes.begin() + 24);
  std::fill_n(bytes.begin() + 20, 4, 0);
  const auto crc = inventoryIndexCrc(bytes);
  for (unsigned at = 0; at < 4; ++at) bytes[20 + at] = static_cast<uint8_t>(crc >> (8 * at));
  hashBytes();
  Transfer transfer(storage, scratch);
  receive(transfer);
  EXPECT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::Invalid);
  EXPECT_EQ(hal.files.at(ACTIVE_COURSE_PATH), original);
  EXPECT_EQ(hal.files.at("/tinta/items.bin"), std::vector<uint8_t>({17}));
  EXPECT_EQ(hal.renames, 0u);
  EXPECT_FALSE(hal.files.contains(TRANSFER_BACKUP));
}
TEST_F(HalCourseTransferTest, ValidatesSameCourseUpdateAgainstCompletedRemovedPack) {
  removeInstalledCourse();
  ASSERT_FALSE(HasFatalFailure());
  bytes[12] ^= 1;
  sealPack();
  auto& hal = inventory_hal_test::state;
  hal.files["/candidate.pack"] = bytes;
  declaration.state.durableOffset = declaration.state.length;
  const auto before = hal.files;
  EXPECT_TRUE(
      storage.validateContent(ACTIVE_COURSE_PATH, "/candidate.pack", declaration.manifest, declaration.state, scratch));
  EXPECT_EQ(hal.files, before);
  EXPECT_FALSE(storage.validateContent(ACTIVE_COURSE_PATH, "/candidate.pack", declaration.manifest, scratch));
  declaration.state.storageGeneration[0] ^= 1;
  EXPECT_FALSE(
      storage.validateContent(ACTIVE_COURSE_PATH, "/candidate.pack", declaration.manifest, declaration.state, scratch));
  EXPECT_EQ(hal.files, before);
}
TEST_F(HalCourseTransferTest, RejectsReassignedIdentityAgainstCompletedRemovedPack) {
  addIdentityHistory();
  removeInstalledCourse();
  ASSERT_FALSE(HasFatalFailure());
  bytes.back() = 2;
  sealPack();
  auto& hal = inventory_hal_test::state;
  hal.files["/candidate.pack"] = bytes;
  declaration.state.durableOffset = declaration.state.length;
  const auto before = hal.files;
  EXPECT_FALSE(
      storage.validateContent(ACTIVE_COURSE_PATH, "/candidate.pack", declaration.manifest, declaration.state, scratch));
  EXPECT_EQ(hal.files, before);
}
TEST_F(HalCourseTransferTest, RemovedBaselineValidationRejectsUnfinishedTransferAndCorruptProof) {
  removeInstalledCourse();
  ASSERT_FALSE(HasFatalFailure());
  auto& hal = inventory_hal_test::state;
  hal.files["/candidate.pack"] = bytes;
  const auto before = hal.files;
  EXPECT_FALSE(
      storage.validateContent(ACTIVE_COURSE_PATH, "/candidate.pack", declaration.manifest, declaration.state, scratch));
  declaration.state.durableOffset = declaration.state.length;
  declaration.state.phase = TransferPhase::Installing;
  EXPECT_FALSE(
      storage.validateContent(ACTIVE_COURSE_PATH, "/candidate.pack", declaration.manifest, declaration.state, scratch));
  declaration.state.phase = TransferPhase::Receiving;
  companion_memory_test::internal.freeBytes = 50 * 1024;
  EXPECT_FALSE(
      storage.validateContent(ACTIVE_COURSE_PATH, "/candidate.pack", declaration.manifest, declaration.state, scratch));
  EXPECT_EQ(hal.files, before);
  companion_memory_test::internal = {1024 * 1024, 1024 * 1024, 1024 * 1024, 1024 * 1024};
  hal.files.at(COURSE_REMOVAL_PROOF_PATH)[0] ^= 1;
  const auto corrupt = hal.files;
  EXPECT_FALSE(
      storage.validateContent(ACTIVE_COURSE_PATH, "/candidate.pack", declaration.manifest, declaration.state, scratch));
  EXPECT_EQ(hal.files, corrupt);
}
TEST_F(HalCourseTransferTest, RejectsLanguageChangeAgainstCompletedRemovedPack) {
  removeInstalledCourse();
  ASSERT_FALSE(HasFatalFailure());
  std::fill_n(bytes.begin() + 24, 8, 0);
  std::copy_n("fr", 2, bytes.begin() + 24);
  sealPack();
  auto& hal = inventory_hal_test::state;
  hal.files["/candidate.pack"] = bytes;
  declaration.state.durableOffset = declaration.state.length;
  const auto before = hal.files;
  EXPECT_FALSE(
      storage.validateContent(ACTIVE_COURSE_PATH, "/candidate.pack", declaration.manifest, declaration.state, scratch));
  EXPECT_EQ(hal.files, before);
}
TEST_F(HalCourseTransferTest, RefusesDifferentLanguageWithinBoundCourseBeforeReplacingProgress) {
  auto& hal = inventory_hal_test::state;
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  hal.files["/tinta/items.bin"] = {17};
  std::array<uint8_t, COURSE_BINDING_SIZE> binding;
  ASSERT_EQ(encodeCourseBinding(declaration.manifest, binding), binding.size());
  hal.files[COURSE_BINDING_PATH] = {binding.begin(), binding.end()};
  const auto original = bytes;
  std::fill_n(bytes.begin() + 24, 8, 0);
  std::copy_n("fr", 2, bytes.begin() + 24);
  std::fill_n(bytes.begin() + 20, 4, 0);
  const auto crc = inventoryIndexCrc(bytes);
  for (unsigned at = 0; at < 4; ++at) bytes[20 + at] = static_cast<uint8_t>(crc >> (8 * at));
  hashBytes();
  Transfer transfer(storage, scratch);
  receive(transfer);
  EXPECT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::Invalid);
  EXPECT_EQ(hal.files.at(ACTIVE_COURSE_PATH), original);
  EXPECT_EQ(hal.files.at("/tinta/items.bin"), std::vector<uint8_t>({17}));
  EXPECT_EQ(hal.files.at(COURSE_BINDING_PATH), std::vector<uint8_t>(binding.begin(), binding.end()));
  EXPECT_EQ(hal.renames, 0u);
}
TEST_F(HalCourseTransferTest, AllowsLocaleCaseChangeWithinBoundCourse) {
  auto& hal = inventory_hal_test::state;
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  hal.files["/tinta/items.bin"] = {17};
  std::array<uint8_t, COURSE_BINDING_SIZE> binding;
  ASSERT_EQ(encodeCourseBinding(declaration.manifest, binding), binding.size());
  hal.files[COURSE_BINDING_PATH] = {binding.begin(), binding.end()};
  for (size_t at = 24; at < 32; ++at)
    if (bytes[at] >= 'a' && bytes[at] <= 'z') bytes[at] -= 'a' - 'A';
  std::fill_n(bytes.begin() + 20, 4, 0);
  const auto crc = inventoryIndexCrc(bytes);
  for (unsigned at = 0; at < 4; ++at) bytes[20 + at] = static_cast<uint8_t>(crc >> (8 * at));
  hashBytes();
  Transfer transfer(storage, scratch);
  receive(transfer);
  ASSERT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::Ok);
  EXPECT_EQ(hal.files.at(ACTIVE_COURSE_PATH), bytes);
  EXPECT_EQ(hal.files.at("/tinta/items.bin"), std::vector<uint8_t>({17}));
}
TEST_F(HalCourseTransferTest, RefusesChangedLegacyContentBeforeReplacingLearningState) {
  auto& hal = inventory_hal_test::state;
  const auto original = bytes;
  hal.files[ACTIVE_COURSE_PATH] = original;
  hal.files["/tinta/items.bin"] = {17};
  std::array<uint8_t, COURSE_BINDING_SIZE> binding;
  ASSERT_EQ(encodeCourseBinding(declaration.manifest, binding), binding.size());
  hal.files[COURSE_BINDING_PATH] = {binding.begin(), binding.end()};
  static constexpr char NAME[] = "Luis";
  auto name = std::search(bytes.begin(), bytes.end(), NAME, NAME + 4);
  ASSERT_NE(name, bytes.end());
  name[3] = 'z';
  std::fill_n(bytes.begin() + 20, 4, 0);
  const auto crc = inventoryIndexCrc(bytes);
  for (unsigned at = 0; at < 4; ++at) bytes[20 + at] = static_cast<uint8_t>(crc >> (8 * at));
  hashBytes();
  Transfer transfer(storage, scratch);
  receive(transfer);
  EXPECT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::Invalid);
  EXPECT_EQ(hal.files.at(ACTIVE_COURSE_PATH), original);
  EXPECT_EQ(hal.files.at("/tinta/items.bin"), std::vector<uint8_t>({17}));
  EXPECT_EQ(hal.files.at(COURSE_BINDING_PATH), std::vector<uint8_t>(binding.begin(), binding.end()));
  EXPECT_EQ(hal.renames, 0u);
}
TEST_F(HalCourseTransferTest, RefusesDeclaredFormatMismatchAndUnsupportedDestination) {
  declaration.manifest.formatVersion = 2;
  Transfer transfer(storage, scratch);
  receive(transfer, false);
  EXPECT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::Invalid);
  EXPECT_FALSE(inventory_hal_test::state.files.contains(ACTIVE_COURSE_PATH));
  declaration.manifest.formatVersion = 1;
  inventory_hal_test::state.files[TRANSFER_STAGE] = bytes;
  EXPECT_FALSE(storage.validateContent("/other.pack", TRANSFER_STAGE, declaration.manifest, scratch));
}

TEST_F(HalCourseTransferTest, RecoversBindingSyncFailureBeforeReleasingOriginalPack) {
  inventory_hal_test::state.files[ACTIVE_COURSE_PATH] = bytes;
  inventory_hal_test::state.files["/tinta/items.bin"] = {99};
  Transfer transfer(storage, scratch);
  receive(transfer);
  inventory_hal_test::state.failSyncPath = COURSE_BINDING_STAGE;
  EXPECT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::IoError);
  EXPECT_EQ(transfer.current()->phase, TransferPhase::Installing);
  EXPECT_TRUE(inventory_hal_test::state.files.contains(TRANSFER_BACKUP));
  inventory_hal_test::state.failSyncPath.clear();
  Transfer reboot(storage, scratch);
  ASSERT_EQ(reboot.recover(generation), TransferResult::Ok);
  EXPECT_EQ(reboot.current()->phase, TransferPhase::Committed);
  EXPECT_FALSE(inventory_hal_test::state.files.contains(TRANSFER_BACKUP));
  ContentManifest binding;
  ASSERT_TRUE(decodeCourseBinding(inventory_hal_test::state.files.at(COURSE_BINDING_PATH), binding));
  EXPECT_EQ(binding, declaration.manifest);
  EXPECT_EQ(inventory_hal_test::state.files.at("/tinta/items.bin"), std::vector<uint8_t>({99}));
}

namespace {
class OtherInventoryResolver final : public InventoryFileResolver {
 public:
  unsigned resolved = 0, verified = 0;
  InventoryFileDecision resolve(const char*, HalFile&, ContentManifest& metadata) override {
    ++resolved;
    metadata = {};
    return InventoryFileDecision::Include;
  }
  bool verifyHashed(const char*, const ContentManifest&) override {
    ++verified;
    return false;
  }
};
}  // namespace
TEST_F(HalCourseTransferTest, InventoryCourseBindingMustMatchActualHashedPack) {
  auto& files = inventory_hal_test::state.files;
  files[ACTIVE_COURSE_PATH] = bytes;
  std::array<uint8_t, COURSE_BINDING_SIZE> binding;
  ASSERT_EQ(encodeCourseBinding(declaration.manifest, binding), binding.size());
  files[COURSE_BINDING_PATH] = {binding.begin(), binding.end()};
  auto pack = makeUniqueNoThrow<tinta::core::pack::Pack>();
  ASSERT_TRUE(pack);
  OtherInventoryResolver other;
  HalInventoryCourseResolver resolver(storage, *pack, other, scratch);
  HalFile file(ACTIVE_COURSE_PATH);
  ContentManifest manifest;
  ASSERT_EQ(resolver.resolve(ACTIVE_COURSE_PATH, file, manifest), InventoryFileDecision::Include);
  ASSERT_TRUE(hashInventoryFile(file, scratch, manifest.length, manifest.contentHash));
  EXPECT_TRUE(resolver.verifyHashed(ACTIVE_COURSE_PATH, manifest));
  manifest.contentHash[0] ^= 1;
  EXPECT_FALSE(resolver.verifyHashed(ACTIVE_COURSE_PATH, manifest));
  manifest = declaration.manifest;
  ++manifest.length;
  EXPECT_FALSE(resolver.verifyHashed(ACTIVE_COURSE_PATH, manifest));
  manifest = declaration.manifest;
  manifest.logicalIdentity[0] ^= 1;
  EXPECT_FALSE(resolver.verifyHashed(ACTIVE_COURSE_PATH, manifest));
  EXPECT_EQ(other.resolved, 0u);
  files["/TINTA/COURSE.PACK"] = bytes;
  HalFile alias("/TINTA/COURSE.PACK");
  ASSERT_EQ(resolver.resolve("/TINTA/COURSE.PACK", alias, manifest), InventoryFileDecision::Include);
  ASSERT_TRUE(hashInventoryFile(alias, scratch, manifest.length, manifest.contentHash));
  EXPECT_TRUE(resolver.verifyHashed("/TINTA/COURSE.PACK", manifest));
  EXPECT_FALSE(resolver.verifyHashed("/TINTA/COURSE.PACK.extra", manifest));
}
TEST_F(HalCourseTransferTest, InventoryLegacyCourseRemainsUnboundAndBadBindingFails) {
  auto& files = inventory_hal_test::state.files;
  files[ACTIVE_COURSE_PATH] = bytes;
  auto pack = makeUniqueNoThrow<tinta::core::pack::Pack>();
  ASSERT_TRUE(pack);
  OtherInventoryResolver other;
  HalInventoryCourseResolver resolver(storage, *pack, other, scratch);
  HalFile file(ACTIVE_COURSE_PATH);
  ContentManifest manifest;
  ASSERT_EQ(resolver.resolve(ACTIVE_COURSE_PATH, file, manifest), InventoryFileDecision::Include);
  EXPECT_EQ(manifest.kind, ContentKind::Course);
  EXPECT_EQ(manifest.logicalIdentity, Identity{});
  EXPECT_EQ(manifest.formatVersion, 1u);
  ASSERT_TRUE(hashInventoryFile(file, scratch, manifest.length, manifest.contentHash));
  EXPECT_TRUE(resolver.verifyHashed(ACTIVE_COURSE_PATH, manifest));
  files[COURSE_BINDING_PATH] = {1, 2, 3};
  EXPECT_EQ(resolver.resolve(ACTIVE_COURSE_PATH, file, manifest), InventoryFileDecision::Error);
  files.erase(COURSE_BINDING_PATH);
  files[ACTIVE_COURSE_PATH][20] ^= 1;
  EXPECT_EQ(resolver.resolve(ACTIVE_COURSE_PATH, file, manifest), InventoryFileDecision::Error);
  EXPECT_EQ(resolver.resolve("/books/other.epub", file, manifest), InventoryFileDecision::Include);
  EXPECT_FALSE(resolver.verifyHashed("/books/other.epub", manifest));
  EXPECT_EQ(other.resolved, 1u);
  EXPECT_EQ(other.verified, 1u);
}

TEST_F(HalCourseTransferTest, ActualCourseInventoryPipelinePreservesSnapshotWhenBindingBecomesStale) {
  auto& state = inventory_hal_test::state;
  state.directories["/"] = {{"tinta", true}};
  state.directories["/tinta"] = {{"course.pack", false}};
  state.files[ACTIVE_COURSE_PATH] = bytes;
  std::array<uint8_t, COURSE_BINDING_SIZE> binding;
  ASSERT_EQ(encodeCourseBinding(declaration.manifest, binding), binding.size());
  state.files[COURSE_BINDING_PATH] = {binding.begin(), binding.end()};
  auto pack = makeUniqueNoThrow<tinta::core::pack::Pack>();
  ASSERT_TRUE(pack);
  OtherInventoryResolver delegate;
  HalInventoryCourseResolver resolver(storage, *pack, delegate,
                                      std::span(scratch).first(HalInventoryBuildSession::HASH_SIZE));
  auto session = makeUniqueNoThrow<HalInventoryBuildSession>(storage, resolver, std::span(scratch));
  ASSERT_TRUE(session);
  uint64_t revision = 42;
  ASSERT_EQ(session->build(generation, revision), InventoryPublicationResult::Ok);
  EXPECT_EQ(revision, 1u);
  HalInventoryIndexStorage reader;
  ASSERT_TRUE(reader.open(InventoryPublication::INDEX));
  IndexedInventoryCatalog catalog(reader, scratch);
  ASSERT_TRUE(catalog.open(generation));
  EXPECT_EQ(catalog.count(), 1u);
  ContentManifest installed;
  ASSERT_TRUE(catalog.read(0, installed));
  EXPECT_EQ(installed, declaration.manifest);
  ASSERT_TRUE(reader.close());
  const auto oldIndex = state.files.at(InventoryPublication::INDEX);
  const auto oldPaths = state.files.at(InventoryPublication::PATHS);
  auto stale = declaration.manifest;
  stale.contentHash[0] ^= 1;
  ASSERT_EQ(encodeCourseBinding(stale, binding), binding.size());
  state.files[COURSE_BINDING_PATH] = {binding.begin(), binding.end()};
  EXPECT_EQ(session->build(generation, revision), InventoryPublicationResult::IoError);
  EXPECT_EQ(revision, 1u);
  EXPECT_EQ(state.files.at(InventoryPublication::INDEX), oldIndex);
  EXPECT_EQ(state.files.at(InventoryPublication::PATHS), oldPaths);
}

TEST_F(HalCourseTransferTest, StartupPublicationSkipsAbsentIntentAndRejectsMalformedIntentBeforeIdentityWrites) {
  class Identities final : public IdentityStorage {
   public:
    unsigned calls = 0;
    bool hardwareIdentity(Identity&) override {
      ++calls;
      return false;
    }
    bool cardIdentity(Identity&) override {
      ++calls;
      return false;
    }
    IdentityRead readBinding(std::span<uint8_t>) override {
      ++calls;
      return IdentityRead::Error;
    }
    bool writeBinding(std::span<const uint8_t>) override {
      ++calls;
      return false;
    }
    IdentityRead readMarker(Identity&) override {
      ++calls;
      return IdentityRead::Error;
    }
    bool createMarker(const Identity&) override {
      ++calls;
      return false;
    }
    bool randomIdentity(Identity&) override {
      ++calls;
      return false;
    }
  } identities;
  inventory_hal_test::state.enumerateFileMap = true;
  Identity course{};
  course.fill(7);
  std::array<char, COURSE_STATE_DIRECTORY_SIZE> root{};
  ASSERT_TRUE(courseStateDirectory(course, root));
  ASSERT_TRUE(Storage.ensureDirectoryExists(root.data()));
  HalTintaDerivedStartupRecovery recovery(course, identities);
  ASSERT_TRUE(recovery.run());
  EXPECT_EQ(identities.calls, 0u);
  std::array<char, COURSE_STATE_PATH_SIZE> intent{};
  ASSERT_TRUE(tintaDerivedRecordPath(course, TintaDerivedRecord::Intent, intent));
  auto& state = inventory_hal_test::state;
  state.files[intent.data()] = {1, 2, 3};
  const auto preserved = state.files;
  EXPECT_FALSE(recovery.run());
  EXPECT_EQ(identities.calls, 0u);
  EXPECT_EQ(state.files, preserved);
  std::ifstream manifestInput(std::string(COMPANION_FIXTURE_DIR) + "/TintaDerivedManifest-v1.fixture",
                              std::ios::binary);
  std::vector<uint8_t> valid{std::istreambuf_iterator<char>(manifestInput), std::istreambuf_iterator<char>()};
  ASSERT_EQ(valid.size(), TINTA_DERIVED_MANIFEST_SIZE);
  std::copy(course.begin(), course.end(), valid.begin() + 4);
  tinta_body_detail::write(valid, 328, binary_record::crc32(valid.data(), 328), 4);
  state.files[intent.data()] = valid;
  const auto withoutBinding = state.files;
  EXPECT_FALSE(recovery.run());
  EXPECT_EQ(identities.calls, 0u);
  EXPECT_EQ(state.files, withoutBinding);
  state.files.erase(intent.data());
  state.statErrorPath = root.data();
  EXPECT_FALSE(recovery.run());
  EXPECT_EQ(identities.calls, 0u);
  state.statErrorPath.clear();
  ASSERT_TRUE(recovery.run());
  EXPECT_EQ(identities.calls, 0u);
}

TEST_F(HalCourseTransferTest, StartupRecoversProvenSnapshotAfterPartialInstallAndRejectsChangedCard) {
  class Identities final : public IdentityStorage {
   public:
    std::array<uint8_t, IDENTITY_RECORD_SIZE> binding{};
    Identity marker{};
    bool saved = false;
    uint8_t card = 2, random = 5;
    bool hardwareIdentity(Identity& output) override {
      output.fill(1);
      return true;
    }
    bool cardIdentity(Identity& output) override {
      output.fill(card);
      return true;
    }
    IdentityRead readBinding(std::span<uint8_t> output) override {
      if (!saved) return IdentityRead::Missing;
      std::copy(binding.begin(), binding.end(), output.begin());
      return IdentityRead::Present;
    }
    bool writeBinding(std::span<const uint8_t> input) override {
      std::copy(input.begin(), input.end(), binding.begin());
      saved = true;
      return true;
    }
    IdentityRead readMarker(Identity& output) override {
      if (!marker[0]) return IdentityRead::Missing;
      output = marker;
      return IdentityRead::Present;
    }
    bool createMarker(const Identity& value) override {
      marker = value;
      return true;
    }
    bool randomIdentity(Identity& output) override {
      output.fill(random++);
      return true;
    }
  };
  for (const bool changedCard : {false, true}) {
    SCOPED_TRACE(changedCard);
    auto& state = inventory_hal_test::state;
    state = {};
    state.enumerateFileMap = true;
    Identities identities;
    ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
    HalJournalMergeStartupRecovery emptyMergeRecovery;
    ASSERT_TRUE(emptyMergeRecovery.run(identities));
    EXPECT_FALSE(identities.saved);
    state.files[HalJournalMergeRecordStore::INTENT] = {1, 2, 3};
    const auto malformedMerge = state.files;
    EXPECT_FALSE(emptyMergeRecovery.run(identities));
    EXPECT_FALSE(identities.saved);
    EXPECT_EQ(state.files, malformedMerge);
    state.files.erase(HalJournalMergeRecordStore::INTENT);
    IdentityState identity;
    ASSERT_EQ(provisionIdentity(identities, identity), IdentityResult::Ok);
    const auto course = declaration.manifest.logicalIdentity;
    state.files[ACTIVE_COURSE_PATH] = bytes;
    std::array<uint8_t, COURSE_BINDING_SIZE> binding{};
    ASSERT_EQ(encodeCourseBinding(declaration.manifest, binding), binding.size());
    state.files[COURSE_BINDING_PATH] = {binding.begin(), binding.end()};
    HalInventoryIndexStorage sourceStorage;
    ASSERT_TRUE(sourceStorage.open(ACTIVE_COURSE_PATH));
    StoredCourseSource source(sourceStorage);
    ASSERT_TRUE(source.attach());
    tinta::core::pack::Pack pack;
    ASSERT_EQ(validateCourseCandidate(pack, source, scratch), CourseValidationResult::Ok);
    TintaPackSubjectCatalog catalog(pack, source);
    ASSERT_TRUE(catalog.prepare(scratch));
    const auto uid = pack.uidAt(0);
    ASSERT_NE(uid, 0u);
    {
      HalTintaJournalStorage journalStorage;
      std::array<uint8_t, 512> journalScratch{};
      TintaJournal journal(journalStorage, journalScratch);
      ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
      TintaBody body;
      body.kind = EventKind::Star;
      body.course = course;
      body.uid = uid;
      body.enabled = true;
      std::array<uint8_t, MAX_TINTA_BODY_SIZE> encoded{};
      const auto length = encodeTintaBody(body, encoded);
      SyncEvent event;
      event.identity = {identity.device, identity.eventEpoch, 1};
      event.storageGeneration = identity.storageGeneration;
      event.kind = body.kind;
      event.resource = declaration.manifest.contentHash;
      ASSERT_TRUE(journalStorage.digest(std::span(encoded).first(length), event.bodyHash));
      ASSERT_EQ(journal.append(event, std::span(encoded).first(length)), TintaJournalResult::Ok);
    }
    HalTintaReplaySession replay;
    ASSERT_TRUE(replay.run(course, catalog));
    HalTintaReplayExport output;
    ASSERT_TRUE(output.run(*replay.workingStore(), course, 5, scratch));
    Identity snapshot{};
    snapshot.fill(9);
    std::array<uint8_t, TINTA_DERIVED_MANIFEST_SIZE> manifest{};
    ASSERT_TRUE(output.manifest(identity.storageGeneration, declaration.manifest.contentHash, *replay.journalFrontier(),
                                snapshot, 1, manifest));
    ASSERT_TRUE(replay.workingStore()->close());
    pack.close();
    ASSERT_TRUE(sourceStorage.close());
    std::array<char, COURSE_STATE_PATH_SIZE> intent{}, candidate{}, active{};
    ASSERT_TRUE(tintaDerivedRecordPath(course, TintaDerivedRecord::Intent, intent));
    state.files[intent.data()] = {manifest.begin(), manifest.end()};
    ASSERT_TRUE(tintaDerivedFilePath(course, TintaDerivedFile::Items, TintaDerivedRole::Candidate, candidate));
    ASSERT_TRUE(tintaDerivedFilePath(course, TintaDerivedFile::Items, TintaDerivedRole::Active, active));
    ASSERT_TRUE(Storage.rename(candidate.data(), active.data()));
    const auto interrupted = state.files;
    if (changedCard) identities.card = 3;
    HalTintaDerivedStartupRecovery recovery(course, identities);
    if (changedCard) {
      EXPECT_FALSE(recovery.run());
      EXPECT_EQ(state.files, interrupted);
      continue;
    }
    ASSERT_TRUE(recovery.run());
    EXPECT_FALSE(state.files.contains(intent.data()));
    for (unsigned at = 0; at < 5; ++at) {
      ASSERT_TRUE(tintaDerivedFilePath(course, static_cast<TintaDerivedFile>(at), TintaDerivedRole::Active, active));
      EXPECT_TRUE(state.files.contains(active.data()));
    }
    const auto committed = state.files;
    ASSERT_TRUE(recovery.run());
    EXPECT_EQ(state.files, committed);
    ASSERT_TRUE(sourceStorage.open(ACTIVE_COURSE_PATH));
    ASSERT_TRUE(source.attach());
    ASSERT_EQ(validateCourseCandidate(pack, source, scratch), CourseValidationResult::Ok);
    ASSERT_TRUE(catalog.prepare(scratch));
    {
      HalTintaJournalMergeCommitContext local;
      auto foreign = course;
      foreign[0] ^= 1;
      const auto unchanged = state.files;
      EXPECT_FALSE(local.reconcileLocalHistory(foreign, identity.storageGeneration, identities));
      EXPECT_EQ(state.files, unchanged);
      std::array<char, COURSE_STATE_PATH_SIZE> receiptPath{};
      ASSERT_TRUE(tintaDerivedRecordPath(course, TintaDerivedRecord::Receipt, receiptPath));
      const auto savedReceipt = state.files.at(receiptPath.data());
      const auto savedHistory = state.files.at(TINTA_JOURNAL_EVENTS);
      state.files.erase(receiptPath.data());
      EXPECT_FALSE(local.reconcileLocalHistory(course, identity.storageGeneration, identities));
      EXPECT_EQ(state.files.at(TINTA_JOURNAL_EVENTS), savedHistory);
      EXPECT_FALSE(state.files.contains(receiptPath.data()));
      state.files[receiptPath.data()] = savedReceipt;
    }
    {
      HalTintaJournalStorage localStorage;
      std::array<uint8_t, 1024> localScratch{};
      TintaJournal local(localStorage, localScratch);
      ASSERT_EQ(local.open(), TintaJournalResult::Ok);
      TintaBody star;
      star.kind = EventKind::Star;
      star.course = course;
      star.uid = uid;
      star.enabled = true;
      std::array<uint8_t, MAX_TINTA_BODY_SIZE> encoded{};
      const auto length = encodeTintaBody(star, encoded);
      SyncEvent edit;
      edit.identity = {identity.device, identity.eventEpoch, 2};
      edit.storageGeneration = identity.storageGeneration;
      edit.kind = star.kind;
      edit.resource = declaration.manifest.contentHash;
      ASSERT_TRUE(localStorage.digest(std::span(encoded).first(length), edit.bodyHash));
      ASSERT_EQ(local.append(edit, std::span(encoded).first(length)), TintaJournalResult::Ok);
    }
    {
      HalTintaJournalMergeCommitContext local;
      ASSERT_TRUE(local.reconcileLocalHistory(course, identity.storageGeneration, identities));
      HalTintaDerivedRecordReader records(course, scratch);
      std::array<uint8_t, TINTA_DERIVED_MANIFEST_SIZE> current{};
      ASSERT_EQ(records.load(TintaDerivedRecord::Receipt, current), TintaDerivedRecordLoad::Loaded);
      TintaDerivedManifestView currentView;
      ASSERT_TRUE(currentView.decode(current));
      EXPECT_EQ(currentView.revision(), 2U);
    }
    JournalMergeIntent merge;
    merge.generation = identity.storageGeneration;
    merge.transaction.fill(10);
    merge.owner.fill(11);
    SyncEvent incoming;
    incoming.identity = {identity.device, identity.eventEpoch, 3};
    incoming.storageGeneration = identity.storageGeneration;
    incoming.kind = EventKind::Suspension;
    incoming.resource = declaration.manifest.contentHash;
    TintaBody incomingBody;
    incomingBody.kind = incoming.kind;
    incomingBody.course = course;
    incomingBody.uid = uid;
    incomingBody.enabled = true;
    std::array<uint8_t, MAX_TINTA_BODY_SIZE> incomingBytes{};
    const auto incomingLength = encodeTintaBody(incomingBody, incomingBytes);
    {
      HalJournalCausalAuditSession original;
      ASSERT_TRUE(original.run(&merge.previous.frontier));
      merge.previous.count = original.recordCount();
      merge.previous.recordSize = original.recordSize();
      HalTintaJournalStorage candidateStorage(TintaJournalLocation::MergeCandidate);
      std::array<uint8_t, 1024> candidateScratch{};
      TintaJournal journal(candidateStorage, candidateScratch);
      ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
      ASSERT_TRUE(original.copyTo(journal));
      ASSERT_TRUE(candidateStorage.digest(std::span(incomingBytes).first(incomingLength), incoming.bodyHash));
      ASSERT_EQ(journal.append(incoming, std::span(incomingBytes).first(incomingLength)), TintaJournalResult::Ok);
    }
    {
      HalJournalCausalAuditSession candidateAudit(TintaJournalLocation::MergeCandidate);
      ASSERT_TRUE(candidateAudit.run(&merge.merged.frontier, &course, &catalog));
      merge.merged.count = candidateAudit.recordCount();
      merge.merged.recordSize = candidateAudit.recordSize();
    }
    for (const auto* path :
         {MERGE_TINTA_JOURNAL_PATHS.events, MERGE_TINTA_JOURNAL_PATHS.headerA, MERGE_TINTA_JOURNAL_PATHS.headerB})
      state.files.erase(path);
    state.directories.erase(MERGE_TINTA_JOURNAL_PATHS.directory);
    {
      HalJournalMergeCandidateSession session;
      ASSERT_EQ(session.begin(merge, merge.generation), TintaJournalResult::Ok);
      ASSERT_EQ(session.append(incoming, std::span(incomingBytes).first(incomingLength)), TintaJournalResult::Ok);
      std::array<uint8_t, JOURNAL_MERGE_INTENT_SIZE> mergeBytes{};
      ASSERT_TRUE(encodeJournalMergeIntent(merge, mergeBytes));
      HalTintaJournalMergeCommitContext context;
      auto wrongGeneration = merge.generation;
      wrongGeneration[0] ^= 1;
      const auto beforeValidation = state.files;
      EXPECT_FALSE(context.prepare(mergeBytes, wrongGeneration));
      EXPECT_EQ(state.files, beforeValidation);
      state.files[ACTIVE_COURSE_PATH].back() ^= 1;
      const auto changedPack = state.files;
      EXPECT_FALSE(context.prepare(mergeBytes, merge.generation));
      EXPECT_EQ(state.files, changedPack);
      state.files[ACTIVE_COURSE_PATH] = bytes;
      ASSERT_TRUE(tintaDerivedFilePath(course, TintaDerivedFile::Items, TintaDerivedRole::Active, active));
      const auto originalItems = state.files.at(active.data());
      state.files[active.data()].back() ^= 1;
      const auto changedBaseline = state.files;
      EXPECT_FALSE(context.prepare(mergeBytes, merge.generation));
      EXPECT_EQ(state.files, changedBaseline);
      EXPECT_EQ(context.catalog(), nullptr);
      state.files[active.data()] = originalItems;
      std::array<uint8_t, JOURNAL_MERGE_READINESS_REQUEST_SIZE> readiness{};
      readiness[0] = 'J';
      readiness[1] = 'R';
      readiness[2] = 'D';
      readiness[3] = 1;
      std::copy(merge.generation.begin(), merge.generation.end(), readiness.begin() + 4);
      tinta_body_detail::write(readiness, 20, merge.previous.count, 4);
      tinta_body_detail::write(readiness, 24, merge.previous.recordSize, 2);
      std::copy(merge.previous.frontier.begin(), merge.previous.frontier.end(), readiness.begin() + 28);
      std::array<uint8_t, JOURNAL_MERGE_READINESS_REPLY_SIZE> readinessReply{};
      ASSERT_EQ(context.readinessReply(readiness, merge.generation, readinessReply), readinessReply.size());
      EXPECT_EQ(readinessReply[4], static_cast<uint8_t>(JournalMergeReadiness::Ready));
      state.files[ACTIVE_COURSE_PATH].back() ^= 1;
      ASSERT_EQ(context.readinessReply(readiness, merge.generation, readinessReply), readinessReply.size());
      EXPECT_EQ(readinessReply[4], static_cast<uint8_t>(JournalMergeReadiness::Unavailable));
      state.files[ACTIVE_COURSE_PATH] = bytes;
      state.files[active.data()].back() ^= 1;
      ASSERT_EQ(context.readinessReply(readiness, merge.generation, readinessReply), readinessReply.size());
      EXPECT_EQ(readinessReply[4], static_cast<uint8_t>(JournalMergeReadiness::Unavailable));
      state.files[active.data()] = originalItems;
      readiness[28] ^= 1;
      ASSERT_EQ(context.readinessReply(readiness, merge.generation, readinessReply), readinessReply.size());
      EXPECT_EQ(readinessReply[4], static_cast<uint8_t>(JournalMergeReadiness::Unavailable));
      readiness[28] ^= 1;
      std::array<char, COURSE_STATE_PATH_SIZE> receiptPath{};
      ASSERT_TRUE(tintaDerivedRecordPath(course, TintaDerivedRecord::Receipt, receiptPath));
      const auto receipt = state.files.at(receiptPath.data());
      state.files.erase(receiptPath.data());
      ASSERT_EQ(context.readinessReply(readiness, merge.generation, readinessReply), readinessReply.size());
      EXPECT_EQ(readinessReply[4], static_cast<uint8_t>(JournalMergeReadiness::MigrationRequired));
      state.files[receiptPath.data()] = receipt;
      state.files[receiptPath.data()][0] ^= 1;
      ASSERT_EQ(context.readinessReply(readiness, merge.generation, readinessReply), readinessReply.size());
      EXPECT_EQ(readinessReply[4], static_cast<uint8_t>(JournalMergeReadiness::Unavailable));
      state.files[receiptPath.data()] = receipt;
      ASSERT_TRUE(context.prepare(mergeBytes, merge.generation));
      ASSERT_EQ(*context.course(), course);
      state.failRenameAfterSource = TINTA_JOURNAL_DIRECTORY;
      ASSERT_EQ(session.commit(merge.generation, context.course(), context.catalog()), TintaJournalResult::IoError);
    }
    state.failRenameAfterSource.clear();
    const auto interruptedMerge = state.files;
    auto changedIdentities = identities;
    changedIdentities.card = 9;
    HalJournalMergeStartupRecovery mergeRecovery;
    auto changedGeneration = identity.storageGeneration;
    changedGeneration[0] ^= 1;
    EXPECT_FALSE(mergeRecovery.run(changedGeneration));
    EXPECT_EQ(state.files, interruptedMerge);
    EXPECT_FALSE(mergeRecovery.run(changedIdentities));
    EXPECT_EQ(state.files, interruptedMerge);
    const auto provisionedBinding = identities.binding;
    ASSERT_TRUE(mergeRecovery.run(identity.storageGeneration));
    EXPECT_EQ(identities.binding, provisionedBinding);
    EXPECT_FALSE(state.files.contains(HalJournalMergeRecordStore::INTENT));
    EXPECT_FALSE(state.files.contains(HalJournalMergeRecordStore::RECEIVING));
    const auto completedMerge = state.files;
    const auto recoveredBinding = identities.binding;
    ASSERT_TRUE(mergeRecovery.run(identities));
    EXPECT_EQ(state.files, completedMerge);
    EXPECT_EQ(identities.binding, recoveredBinding);
    HalJournalMergeRecordStore receivingCheckpoint(JournalMergeRecord::Receiving);
    ASSERT_TRUE(receivingCheckpoint.persist(merge));
    const auto completedWithCheckpoint = state.files;
    EXPECT_FALSE(mergeRecovery.run(changedGeneration));
    EXPECT_EQ(state.files, completedWithCheckpoint);
    const auto mergedJournalBytes = state.files.at(TINTA_JOURNAL_EVENTS);
    state.files[TINTA_JOURNAL_EVENTS].back() ^= 1;
    const auto corruptCompletedJournal = state.files;
    EXPECT_FALSE(mergeRecovery.run(identity.storageGeneration));
    EXPECT_EQ(state.files, corruptCompletedJournal);
    state.files[TINTA_JOURNAL_EVENTS] = mergedJournalBytes;
    ASSERT_TRUE(mergeRecovery.run(identity.storageGeneration));
    EXPECT_FALSE(state.files.contains(HalJournalMergeRecordStore::RECEIVING));
    EXPECT_EQ(state.files, completedMerge);
    auto unfinished = merge;
    unfinished.transaction[0] ^= 1;
    ASSERT_TRUE(receivingCheckpoint.persist(unfinished));
    const auto unrelatedReceiving = state.files;
    const auto unchangedBinding = identities.binding;
    ASSERT_TRUE(mergeRecovery.run(identities));
    EXPECT_EQ(state.files, unrelatedReceiving);
    EXPECT_EQ(identities.binding, unchangedBinding);
    ASSERT_TRUE(receivingCheckpoint.clear(unfinished));
    ASSERT_TRUE(tintaDerivedFilePath(course, TintaDerivedFile::Items, TintaDerivedRole::Active, active));
    const auto baselineItemFile = state.files.at(active.data());
    state.files[active.data()].back() ^= 1;
    const auto locallyChanged = state.files;
    HalTintaMergedJournalReconciliation reconciliation(course, identities);
    EXPECT_FALSE(reconciliation.run());
    EXPECT_EQ(state.files, locallyChanged);
    state.files[active.data()] = baselineItemFile;
    ASSERT_TRUE(reconciliation.run());
    tinta::core::ItemState reconciled;
    ASSERT_TRUE(tinta::core::ItemState::decode(state.files.at(active.data()).data() + 1024, reconciled));
    EXPECT_EQ(reconciled.uid, uid);
    EXPECT_TRUE(reconciled.suspended());
    EXPECT_TRUE(reconciled.flags & tinta::core::item_flag::kStarred);
    const auto appliedFiles = state.files;
    const auto identityBinding = identities.binding;
    ASSERT_TRUE(reconciliation.run());
    EXPECT_EQ(state.files, appliedFiles);
    EXPECT_EQ(identities.binding, identityBinding);
    std::string appliedPath;
    for (const auto& [path, content] : state.files) {
      if (path.starts_with("/.crosspoint/companion/journal-merge-applied-")) {
        appliedPath = path;
        break;
      }
    }
    ASSERT_FALSE(appliedPath.empty());
    state.files.erase(appliedPath);
    const auto publishedItems = state.files.at(active.data());
    state.files[active.data()].back() ^= 1;
    const auto corruptPublished = state.files;
    EXPECT_FALSE(reconciliation.run());
    EXPECT_EQ(state.files, corruptPublished);
    EXPECT_FALSE(state.files.contains(appliedPath));
    state.files[active.data()] = publishedItems;
    ASSERT_TRUE(reconciliation.run());
    EXPECT_TRUE(state.files.contains(appliedPath));
  }
}

TEST_F(HalCourseTransferTest, SelectsDifferentCourseOnlyAfterBothLegacyMigrationsAreProven) {
  auto& hal = inventory_hal_test::state;
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  std::array<uint8_t, COURSE_BINDING_SIZE> binding;
  ASSERT_EQ(encodeCourseBinding(declaration.manifest, binding), binding.size());
  hal.files[COURSE_BINDING_PATH] = {binding.begin(), binding.end()};
  hal.files["/tinta/items.bin"] = {17};
  hal.files["/tinta/starred.bin"] = {23};
  ASSERT_TRUE(prepareMigratedCourseState(storage, declaration.manifest.logicalIdentity, scratch));
  auto next = declaration.manifest;
  next.logicalIdentity[0] = 8;
  ASSERT_EQ(encodeCourseBinding(next, binding), binding.size());
  hal.files[COURSE_BINDING_PATH] = {binding.begin(), binding.end()};
  char previousPath[COURSE_STATE_PATH_SIZE], nextPath[COURSE_STATE_PATH_SIZE];
  ASSERT_TRUE(courseStatePath(declaration.manifest.logicalIdentity, "items.bin", previousPath));
  ASSERT_TRUE(courseStatePath(next.logicalIdentity, "items.bin", nextPath));
  hal.files[nextPath] = {31};
  const auto preserved = hal.files;
  Identity selected{};
  bool bound = false;
  ASSERT_TRUE(selectActiveCourseState(storage, scratch, selected, bound));
  EXPECT_TRUE(bound);
  EXPECT_EQ(selected, next.logicalIdentity);
  EXPECT_EQ(hal.files, preserved);
  EXPECT_EQ(hal.files.at(previousPath), std::vector<uint8_t>({17}));
  EXPECT_EQ(hal.files.at(nextPath), std::vector<uint8_t>({31}));
  hal.files["/tinta/reviews.log"] = {99};
  const auto conflicting = hal.files;
  selected = declaration.manifest.logicalIdentity;
  bound = false;
  EXPECT_FALSE(selectActiveCourseState(storage, scratch, selected, bound));
  EXPECT_EQ(selected, declaration.manifest.logicalIdentity);
  EXPECT_FALSE(bound);
  EXPECT_EQ(hal.files, conflicting);
}

TEST_F(HalCourseTransferTest, ConsentedCourseSwitchInstallsAndRecoversWithSeparateState) {
  auto& hal = inventory_hal_test::state;
  const auto previous = declaration.manifest;
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  std::array<uint8_t, COURSE_BINDING_SIZE> binding;
  ASSERT_EQ(encodeCourseBinding(previous, binding), binding.size());
  hal.files[COURSE_BINDING_PATH] = {binding.begin(), binding.end()};
  hal.files["/tinta/items.bin"] = {17};
  hal.files["/tinta/starred.bin"] = {23};
  declaration.manifest.logicalIdentity[0] = 8;
  std::fill_n(bytes.begin() + 24, 8, 0);
  std::memcpy(bytes.data() + 24, "fr-FR", 5);
  sealPack();
  CourseSwitchRequest consent;
  consent.generation = generation;
  consent.transaction = declaration.state.transaction;
  consent.previousCourse = previous.logicalIdentity;
  consent.previousHash = previous.contentHash;
  consent.nextCourse = declaration.manifest.logicalIdentity;
  consent.nextHash = declaration.manifest.contentHash;
  Transfer transfer(storage, scratch);
  ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
  ASSERT_EQ(transfer.begin(declaration, ACTIVE_COURSE_PATH), TransferResult::Ok);
  std::array<uint8_t, COURSE_SWITCH_REQUEST_SIZE> consentBytes;
  std::array<uint8_t, COURSE_SWITCH_REPLY_SIZE> reply;
  ASSERT_TRUE(encodeCourseSwitchRequest(consent, consentBytes));
  Identity foreignOwner = declaration.state.owner;
  foreignOwner[0] ^= 1;
  const auto unchanged = hal.files;
  ASSERT_EQ(handleCourseSwitch(storage, transfer, generation, foreignOwner, consentBytes, reply, scratch),
            reply.size());
  EXPECT_EQ(reply[0], static_cast<uint8_t>(TransferResult::Unauthorized));
  EXPECT_EQ(hal.files, unchanged);
  ASSERT_EQ(handleCourseSwitch(storage, transfer, generation, declaration.state.owner, consentBytes, reply, scratch),
            reply.size());
  EXPECT_EQ(reply[0], static_cast<uint8_t>(TransferResult::Ok));
  EXPECT_TRUE(std::equal(consent.transaction.begin(), consent.transaction.end(), reply.begin() + 1));
  ASSERT_EQ(handleCourseSwitch(storage, transfer, generation, declaration.state.owner, consentBytes, reply, scratch),
            reply.size());
  EXPECT_EQ(reply[0], static_cast<uint8_t>(TransferResult::Ok));
  auto foreignState = declaration.state;
  foreignState.transaction[0] ^= 1;
  foreignState.durableOffset = foreignState.length;
  const auto before = hal.files;
  EXPECT_FALSE(
      storage.validateContent(ACTIVE_COURSE_PATH, TRANSFER_STAGE, declaration.manifest, foreignState, scratch));
  EXPECT_EQ(hal.files, before);
  for (size_t offset = 0; offset < bytes.size();) {
    const auto chunk = std::span(bytes).subspan(offset, std::min(size_t{1000}, bytes.size() - offset));
    ASSERT_EQ(transfer.append(declaration.state.transaction, declaration.state.owner, offset, chunk),
              TransferResult::Ok);
    offset += chunk.size();
  }
  ASSERT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::Ok);
  EXPECT_FALSE(hal.files.contains(COURSE_SWITCH_INTENT_PATH));
  HalTransferStorage rebootStorage;
  Transfer reboot(rebootStorage, scratch);
  ASSERT_EQ(reboot.recover(generation), TransferResult::Ok);
  Identity selected{};
  bool bound = false;
  ASSERT_TRUE(selectActiveCourseState(rebootStorage, scratch, selected, bound));
  EXPECT_EQ(selected, declaration.manifest.logicalIdentity);
  EXPECT_TRUE(bound);
  char oldPath[COURSE_STATE_PATH_SIZE], nextPath[COURSE_STATE_PATH_SIZE];
  ASSERT_TRUE(courseStatePath(previous.logicalIdentity, "items.bin", oldPath));
  ASSERT_TRUE(courseStatePath(selected, "items.bin", nextPath));
  EXPECT_EQ(hal.files.at(oldPath), std::vector<uint8_t>({17}));
  EXPECT_FALSE(hal.files.contains(nextPath));
  EXPECT_FALSE(hal.files.contains("/tinta/items.bin"));
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(hal.files.at(ACTIVE_COURSE_PATH).data() + 24)), "fr-FR");
}

TEST_F(HalCourseTransferTest, AbortedSwitchRetiresOnlyMatchingConsentAndPreservesOldPack) {
  auto& hal = inventory_hal_test::state;
  const auto previous = declaration.manifest;
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  std::array<uint8_t, COURSE_BINDING_SIZE> binding;
  ASSERT_EQ(encodeCourseBinding(previous, binding), binding.size());
  hal.files[COURSE_BINDING_PATH] = {binding.begin(), binding.end()};
  hal.files["/tinta/items.bin"] = {17};
  const auto oldPack = bytes;
  declaration.manifest.logicalIdentity[0] = 8;
  bytes[12] ^= 1;
  sealPack();
  CourseSwitchRequest consent;
  consent.generation = generation;
  consent.transaction = declaration.state.transaction;
  consent.previousCourse = previous.logicalIdentity;
  consent.previousHash = previous.contentHash;
  consent.nextCourse = declaration.manifest.logicalIdentity;
  consent.nextHash = declaration.manifest.contentHash;
  Transfer transfer(storage, scratch);
  ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
  ASSERT_EQ(transfer.begin(declaration, ACTIVE_COURSE_PATH), TransferResult::Ok);
  CourseSwitchIntent intent(storage, scratch);
  ASSERT_EQ(intent.persist(consent), CourseSwitchIntentResult::Ok);
  auto foreign = declaration.state;
  foreign.phase = TransferPhase::Aborted;
  foreign.transaction[0] ^= 1;
  const auto preserved = hal.files;
  EXPECT_FALSE(storage.finalizeContentMetadata(ACTIVE_COURSE_PATH, declaration.manifest, foreign, scratch));
  EXPECT_EQ(hal.files, preserved);
  hal.failRemoveAfter = true;
  ASSERT_EQ(transfer.abort(declaration.state.transaction, declaration.state.owner), TransferResult::IoError);
  EXPECT_TRUE(hal.files.contains(COURSE_SWITCH_INTENT_PATH));
  EXPECT_FALSE(hal.files.contains(TRANSFER_STAGE));
  hal.failRemoveAfter = false;
  Transfer restarted(storage, scratch);
  ASSERT_EQ(restarted.recover(generation), TransferResult::Ok);
  EXPECT_FALSE(hal.files.contains(COURSE_SWITCH_INTENT_PATH));
  EXPECT_EQ(hal.files.at(ACTIVE_COURSE_PATH), oldPack);
  EXPECT_EQ(hal.files.at("/tinta/items.bin"), std::vector<uint8_t>({17}));
  Transfer reboot(storage, scratch);
  EXPECT_EQ(reboot.recover(generation), TransferResult::Ok);
}

TEST_F(HalCourseTransferTest, CourseSwitchRecoversEveryRenameAndMetadataSyncInterruption) {
  const auto previous = declaration.manifest;
  const auto oldPack = bytes;
  declaration.manifest.logicalIdentity[0] = 8;
  bytes[12] ^= 1;
  sealPack();
  std::array<uint8_t, COURSE_BINDING_SIZE> oldBinding;
  ASSERT_EQ(encodeCourseBinding(previous, oldBinding), oldBinding.size());
  CourseSwitchRequest consent;
  consent.generation = generation;
  consent.transaction = declaration.state.transaction;
  consent.previousCourse = previous.logicalIdentity;
  consent.previousHash = previous.contentHash;
  consent.nextCourse = declaration.manifest.logicalIdentity;
  consent.nextHash = declaration.manifest.contentHash;
  auto exercise = [&](unsigned failedRename, bool after, const char* syncPath) {
    auto& hal = inventory_hal_test::state;
    hal = {};
    HalTransferStorage initialStorage;
    hal.files[ACTIVE_COURSE_PATH] = oldPack;
    hal.files[COURSE_BINDING_PATH] = {oldBinding.begin(), oldBinding.end()};
    hal.files["/tinta/items.bin"] = {17};
    hal.files["/tinta/starred.bin"] = {23};
    char nextPath[COURSE_STATE_PATH_SIZE];
    EXPECT_TRUE(courseStatePath(declaration.manifest.logicalIdentity, "items.bin", nextPath));
    hal.files[nextPath] = {31};
    Transfer transfer(initialStorage, scratch);
    receive(transfer, false);
    CourseSwitchIntent intent(initialStorage, scratch);
    EXPECT_EQ(intent.persist(consent), CourseSwitchIntentResult::Ok);
    const unsigned before = hal.renames;
    if (failedRename) {
      if (after)
        hal.failRenameAfter = before + failedRename;
      else
        hal.failRename = before + failedRename;
    }
    if (syncPath) hal.failSyncPath = syncPath;
    const auto result = transfer.commit(declaration.state.transaction, declaration.state.owner);
    if (failedRename || syncPath)
      EXPECT_NE(result, TransferResult::Ok);
    else
      EXPECT_EQ(result, TransferResult::Ok);
    const unsigned renameCount = hal.renames - before;
    hal.failRename = hal.failRenameAfter = 0;
    hal.failSyncPath.clear();
    HalTransferStorage reopenedStorage;
    Transfer reopened(reopenedStorage, scratch);
    EXPECT_EQ(reopened.recover(generation), TransferResult::Ok);
    EXPECT_EQ(reopened.commit(declaration.state.transaction, declaration.state.owner), TransferResult::Ok);
    EXPECT_EQ(hal.files.at(ACTIVE_COURSE_PATH), bytes);
    EXPECT_EQ(hal.files.at(nextPath), std::vector<uint8_t>({31}));
    char oldPath[COURSE_STATE_PATH_SIZE];
    EXPECT_TRUE(courseStatePath(previous.logicalIdentity, "items.bin", oldPath));
    EXPECT_EQ(hal.files.at(oldPath), std::vector<uint8_t>({17}));
    EXPECT_TRUE(courseStatePath(previous.logicalIdentity, "starred.bin", oldPath));
    EXPECT_EQ(hal.files.at(oldPath), std::vector<uint8_t>({23}));
    EXPECT_FALSE(hal.files.contains(COURSE_SWITCH_INTENT_PATH));
    EXPECT_FALSE(hal.files.contains(TRANSFER_BACKUP));
    EXPECT_FALSE(hal.files.contains(COURSE_BINDING_BACKUP));
    Identity selected{};
    bool bound = false;
    EXPECT_TRUE(selectActiveCourseState(reopenedStorage, scratch, selected, bound));
    EXPECT_EQ(selected, declaration.manifest.logicalIdentity);
    EXPECT_TRUE(bound);
    return renameCount;
  };
  const unsigned renames = exercise(0, false, nullptr);
  ASSERT_GT(renames, 0);
  for (unsigned index = 1; index <= renames; ++index) {
    for (bool after : {false, true}) {
      SCOPED_TRACE(index);
      SCOPED_TRACE(after);
      exercise(index, after, nullptr);
    }
  }
  for (const char* path :
       {COURSE_STATE_MIGRATION_STAGE, COURSE_STATE_MIGRATION_DONE_STAGE, COURSE_MARK_MIGRATION_PATHS.stage,
        COURSE_MARK_MIGRATION_PATHS.doneStage, COURSE_BINDING_STAGE, TRANSFER_JOURNALS[0], TRANSFER_JOURNALS[1]}) {
    SCOPED_TRACE(path);
    exercise(0, false, path);
  }
}

TEST_F(HalCourseTransferTest, FirmwareStagingRequiresValidatorAndPreservesActiveCourse) {
  auto& hal = inventory_hal_test::state;
  const auto course = bytes;
  hal.files[ACTIVE_COURSE_PATH] = course;
  bytes.assign(65536, 0xa5);
  bytes[0] = 0xe9;
  declaration.manifest.kind = ContentKind::Firmware;
  declaration.manifest.logicalIdentity = {};
  declaration.manifest.length = bytes.size();
  hashBytes();
  Transfer transfer(storage, scratch);
  ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
  ASSERT_EQ(transfer.begin(declaration, "/Companion/firmware.bin"), TransferResult::Ok);
  for (size_t offset = 0; offset < bytes.size();) {
    const auto chunk = std::span(bytes).subspan(offset, std::min(size_t{1000}, bytes.size() - offset));
    ASSERT_EQ(transfer.append(declaration.state.transaction, declaration.state.owner, offset, chunk),
              TransferResult::Ok);
    offset += chunk.size();
  }
  EXPECT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::Invalid);
  EXPECT_FALSE(hal.files.contains("/Companion/firmware.bin"));
  EXPECT_EQ(hal.files.at(ACTIVE_COURSE_PATH), course);
  const auto validator = +[](const char* path) {
    HalFile file;
    uint8_t magic = 0;
    return Storage.openFileForRead("TEST", path, file) && file.fileSize64() == 65536 && file.read(&magic, 1) == 1 &&
           magic == 0xe9;
  };
  storage.setFirmwareValidator(validator);
  ASSERT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::Ok);
  EXPECT_EQ(hal.files.at("/Companion/firmware.bin"), bytes);
  EXPECT_EQ(hal.files.at(ACTIVE_COURSE_PATH), course);
  EXPECT_FALSE(hal.files.contains(COURSE_BINDING_PATH));
  HalTransferStorage missingValidator;
  Transfer blocked(missingValidator, scratch);
  EXPECT_EQ(blocked.recover(generation), TransferResult::IoError);
  missingValidator.setFirmwareValidator(validator);
  EXPECT_EQ(blocked.recover(generation), TransferResult::Ok);
  EXPECT_EQ(hal.files.at(ACTIVE_COURSE_PATH), course);
}

TEST_F(HalCourseTransferTest, InstallsValidatedBitmapFontAndRejectsInvalidContentOrRegistryPaths) {
  for (unsigned mode = 0; mode < 5; ++mode) {
    inventory_hal_test::state = {};
    std::ifstream input(FONT_FIXTURE, std::ios::binary);
    ASSERT_TRUE(input.good());
    bytes = {std::istreambuf_iterator<char>(input), {}};
    ASSERT_EQ(bytes.size(), 142u);
    if (mode == 1) bytes[0] ^= 1;
    declaration.manifest.kind = ContentKind::Font;
    declaration.manifest.logicalIdentity = {};
    declaration.manifest.formatVersion = mode == 3 ? 1 : 4;
    declaration.manifest.length = bytes.size();
    hashBytes();
    const char* target = mode == 2 ? "/fonts/Example_14.cpfont" : "/fonts/Example/Example_14.cpfont";
    auto& hal = inventory_hal_test::state;
    const std::vector<uint8_t> previous{9, 8, 7};
    hal.files[target] = previous;
    Transfer transfer(storage, scratch);
    ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
    ASSERT_EQ(transfer.begin(declaration, target), TransferResult::Ok);
    ASSERT_EQ(transfer.append(declaration.state.transaction, declaration.state.owner, 0, bytes), TransferResult::Ok);
    if (mode == 4) hal.failRenameAfterSource = TRANSFER_STAGE;
    const auto result = transfer.commit(declaration.state.transaction, declaration.state.owner);
    if (mode == 0 || mode == 4) {
      ASSERT_EQ(result, mode == 0 ? TransferResult::Ok : TransferResult::IoError);
      hal.failRenameAfterSource.clear();
      EXPECT_EQ(hal.files.at(target), bytes);
      Transfer reopened(storage, scratch);
      ASSERT_EQ(reopened.recover(generation), TransferResult::Ok);
      EXPECT_EQ(reopened.current()->phase, TransferPhase::Committed);
      EXPECT_EQ(hal.files.at(target), bytes);
    } else {
      EXPECT_NE(result, TransferResult::Ok);
      EXPECT_EQ(hal.files.at(target), previous);
    }
  }
}

TEST_F(HalCourseTransferTest, DictionaryArchiveVerificationUsesCacheAfterStageCleanupAndRejectsBadProof) {
  declaration.manifest.kind = ContentKind::Dictionary;
  declaration.manifest.logicalIdentity.fill(0);
  declaration.state.phase = TransferPhase::Installing;
  declaration.state.durableOffset = declaration.state.length;
  inventory_hal_test::state.files[TRANSFER_STAGE] = bytes;
  constexpr const char* destination = "/dictionaries/test/dictionary";
  ASSERT_TRUE(storage.verifyDictionaryArchive(destination, declaration.manifest, declaration.state, scratch));
  HalDictionaryCacheStorage cache;
  DictionaryCachePublication publication(cache, scratch);
  inventory_hal_test::state.files[DICTIONARY_CACHE_CANDIDATE] = bytes;
  ASSERT_EQ(publication.publish(declaration.manifest), DictionaryCacheResult::Ok);
  const std::string cached = publication.publishedPath();
  inventory_hal_test::state.files.at(TRANSFER_STAGE)[0] ^= 1;
  EXPECT_FALSE(storage.verifyDictionaryArchive(destination, declaration.manifest, declaration.state, scratch));
  inventory_hal_test::state.files.erase(TRANSFER_STAGE);
  ASSERT_TRUE(storage.verifyDictionaryArchive(destination, declaration.manifest, declaration.state, scratch));
  declaration.state.phase = TransferPhase::Committed;
  ASSERT_TRUE(storage.verifyDictionaryArchive(destination, declaration.manifest, declaration.state, scratch));
  auto& cachedBytes = inventory_hal_test::state.files.at(cached);
  cachedBytes[0] ^= 1;
  EXPECT_FALSE(storage.verifyDictionaryArchive(destination, declaration.manifest, declaration.state, scratch));
  cachedBytes[0] ^= 1;
  inventory_hal_test::state.readErrorPath = cached;
  EXPECT_FALSE(storage.verifyDictionaryArchive(destination, declaration.manifest, declaration.state, scratch));
  inventory_hal_test::state.readErrorPath.clear();
  auto foreign = declaration.state;
  foreign.contentHash[0] ^= 1;
  EXPECT_FALSE(storage.verifyDictionaryArchive(destination, declaration.manifest, foreign, scratch));
  EXPECT_FALSE(storage.verifyDictionaryArchive("/books/dictionary", declaration.manifest, declaration.state, scratch));
  declaration.state.phase = TransferPhase::Receiving;
  EXPECT_FALSE(storage.verifyDictionaryArchive(destination, declaration.manifest, declaration.state, scratch));
  declaration.state.phase = TransferPhase::Committed;
  inventory_hal_test::state.files.erase(cached);
  inventory_hal_test::state.files[destination] = bytes;
  EXPECT_FALSE(storage.verifyDictionaryArchive(destination, declaration.manifest, declaration.state, scratch));
}

TEST_F(HalCourseTransferTest, DictionaryInstallerDispatchUsesDurablePhasesAndRejectsInvalidContext) {
  struct Installer final : HalDictionaryTransferInstaller {
    unsigned prepared = 0, installed = 0, metadataCalls = 0, finalized = 0;
    bool accept = false;
    bool prepare(const char*, const char* source, const ContentManifest&, const TransferState& state,
                 std::span<uint8_t>) override {
      ++prepared;
      EXPECT_EQ(state.phase, TransferPhase::Receiving);
      EXPECT_STREQ(source, TRANSFER_STAGE);
      return accept;
    }
    bool install(const char*, const ContentManifest&, const TransferState& state, std::span<uint8_t>) override {
      ++installed;
      EXPECT_EQ(state.phase, TransferPhase::Installing);
      return true;
    }
    bool metadata(const char*, const ContentManifest&, const TransferState&, std::span<uint8_t>) override {
      ++metadataCalls;
      return true;
    }
    bool finalize(const char*, const ContentManifest&, const TransferState& state, std::span<uint8_t>) override {
      ++finalized;
      EXPECT_EQ(state.phase, TransferPhase::Committed);
      return true;
    }
  } installer;
  declaration.manifest.kind = ContentKind::Dictionary;
  declaration.manifest.logicalIdentity.fill(0);
  constexpr const char* destination = "/dictionaries/test/dictionary";
  Transfer transfer(storage, scratch);
  ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
  ASSERT_EQ(transfer.begin(declaration, destination), TransferResult::Ok);
  for (size_t offset = 0; offset < bytes.size();) {
    const auto count = std::min<size_t>(512, bytes.size() - offset);
    ASSERT_EQ(transfer.append(declaration.state.transaction, declaration.state.owner, offset,
                              std::span<const uint8_t>(bytes).subspan(offset, count)),
              TransferResult::Ok);
    offset += count;
  }
  storage.setDictionaryInstaller(&installer);
  EXPECT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::Invalid);
  EXPECT_EQ(transfer.current()->phase, TransferPhase::Receiving);
  installer.accept = true;
  ASSERT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::Ok);
  EXPECT_EQ(installer.prepared, 2u);
  EXPECT_EQ(installer.installed, 1u);
  EXPECT_EQ(installer.metadataCalls, 1u);
  EXPECT_EQ(installer.finalized, 1u);
  EXPECT_FALSE(inventory_hal_test::state.files.contains(destination));
  EXPECT_TRUE(inventory_hal_test::state.files.contains(TRANSFER_STAGE));
  auto wrong = *transfer.current();
  wrong.owner.fill(0);
  EXPECT_FALSE(storage.installContentMetadata(destination, declaration.manifest, wrong, scratch));
  EXPECT_FALSE(storage.installDictionaryMembers(destination, declaration.manifest, *transfer.current(), scratch));
  wrong = *transfer.current();
  wrong.transaction.fill(0);
  EXPECT_FALSE(storage.installContentMetadata(destination, declaration.manifest, wrong, scratch));
  wrong = *transfer.current();
  wrong.storageGeneration.fill(0);
  EXPECT_FALSE(storage.finalizeContentMetadata(destination, declaration.manifest, wrong, scratch));
  wrong = *transfer.current();
  --wrong.durableOffset;
  EXPECT_FALSE(storage.installContentMetadata(destination, declaration.manifest, wrong, scratch));
  auto mismatched = declaration.manifest;
  mismatched.contentHash[0] ^= 1;
  EXPECT_FALSE(storage.installContentMetadata(destination, mismatched, *transfer.current(), scratch));
  EXPECT_FALSE(storage.finalizeContentMetadata("/dictionaries/../dictionary", declaration.manifest, *transfer.current(),
                                               scratch));
  EXPECT_FALSE(
      storage.validateContent(destination, TRANSFER_STAGE, declaration.manifest, *transfer.current(), scratch));
  EXPECT_EQ(installer.prepared, 2u);
  EXPECT_EQ(installer.finalized, 1u);
  EXPECT_EQ(installer.installed, 1u);
  EXPECT_EQ(installer.metadataCalls, 1u);
  storage.setDictionaryInstaller(nullptr);
  EXPECT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::IoError);
  EXPECT_EQ(installer.metadataCalls, 1u);
}

TEST_F(HalCourseTransferTest, RetiredDictionaryMetadataVerifiesMembersAndRejectsRemainingStagesWithoutInstaller) {
  const auto fixture = [](const char* name) {
    std::ifstream input(std::string(COMPANION_FIXTURE_DIR) + name, std::ios::binary);
    return std::vector<uint8_t>{std::istreambuf_iterator<char>(input), {}};
  };
  bytes = fixture("DictionaryBundle-plain.fixture");
  ASSERT_FALSE(bytes.empty());
  declaration.manifest.kind = ContentKind::Dictionary;
  declaration.manifest.logicalIdentity.fill(0);
  declaration.manifest.length = bytes.size();
  hashBytes();
  declaration.state.durableOffset = declaration.state.length;
  declaration.state.phase = TransferPhase::Committed;
  auto& hal = inventory_hal_test::state;
  hal.directories["/"] = {{"dictionaries", true}, {".crosspoint", true}};
  hal.directories["/dictionaries"] = {{"es", true}};
  hal.directories["/dictionaries/es"] = {{"stem.idx"}, {"stem.ifo"}, {"stem.dict"}, {"stem.syn"}};
  hal.files["/dictionaries/es/stem.dict"] = {'o', 'n', 'e', 't', 'w', 'o'};
  hal.files["/dictionaries/es/stem.idx"] = fixture("DictionaryIndex-definitions.fixture");
  hal.files["/dictionaries/es/stem.syn"] = fixture("DictionaryIndex-synonyms.fixture");
  const std::string info =
      "StarDict's dict ifo file\nversion=3.0.0\nbookname=Canonical "
      "dictionary\nwordcount=2\nidxfilesize=24\nsynwordcount=1\n";
  hal.files["/dictionaries/es/stem.ifo"] = {info.begin(), info.end()};
  HalDictionaryCacheStorage cache;
  DictionaryCachePublication publication(cache, scratch);
  hal.files[DICTIONARY_CACHE_CANDIDATE] = bytes;
  ASSERT_EQ(publication.publish(declaration.manifest), DictionaryCacheResult::Ok);
  HalDictionaryBindings bindings(cache, scratch);
  constexpr const char* base = "/dictionaries/es/stem";
  const DictionaryArchiveBinding binding{declaration.manifest, declaration.manifest};
  ASSERT_TRUE(bindings.install(base, binding));
  ASSERT_TRUE(bindings.finalizeInstallation(base, binding));
  struct AlreadyInstalled final : HalDictionaryTransferInstaller {
    bool prepare(const char*, const char*, const ContentManifest&, const TransferState&, std::span<uint8_t>) override {
      return true;
    }
    bool install(const char*, const ContentManifest&, const TransferState&, std::span<uint8_t>) override {
      return true;
    }
    bool metadata(const char*, const ContentManifest&, const TransferState&, std::span<uint8_t>) override {
      return true;
    }
    bool finalize(const char*, const ContentManifest&, const TransferState&, std::span<uint8_t>) override {
      return Storage.remove(TRANSFER_STAGE);
    }
  } installer;
  declaration.state.phase = TransferPhase::Receiving;
  declaration.state.durableOffset = 0;
  Transfer transfer(storage, scratch);
  ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
  ASSERT_EQ(transfer.begin(declaration, base), TransferResult::Ok);
  for (size_t offset = 0; offset < bytes.size();) {
    const auto count = std::min<size_t>(512, bytes.size() - offset);
    ASSERT_EQ(transfer.append(declaration.state.transaction, declaration.state.owner, offset,
                              std::span<const uint8_t>(bytes).subspan(offset, count)),
              TransferResult::Ok);
    offset += count;
  }
  storage.setDictionaryInstaller(&installer);
  ASSERT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::Ok);
  storage.setDictionaryInstaller(nullptr);
  ASSERT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::Ok);
  Transfer rebooted(storage, scratch);
  ASSERT_EQ(rebooted.recover(generation), TransferResult::Ok);
  ASSERT_EQ(rebooted.current()->phase, TransferPhase::Committed);
  declaration.state = *rebooted.current();
  const auto saved = hal.files;
  ASSERT_TRUE(storage.installContentMetadata(base, declaration.manifest, declaration.state, scratch));
  ASSERT_TRUE(storage.finalizeContentMetadata(base, declaration.manifest, declaration.state, scratch));
  EXPECT_EQ(hal.files, saved);
  for (const auto path : DICTIONARY_INSTALLATION_TEMPORARIES) {
    hal.files[path] = {1};
    const auto staged = hal.files;
    EXPECT_FALSE(storage.installContentMetadata(base, declaration.manifest, declaration.state, scratch)) << path;
    EXPECT_FALSE(storage.finalizeContentMetadata(base, declaration.manifest, declaration.state, scratch)) << path;
    EXPECT_EQ(hal.files, staged);
    hal.files = saved;
  }
  for (const auto path : DICTIONARY_RETIREMENT_JOURNALS) {
    hal.files[path] = {1};
    EXPECT_FALSE(storage.finalizeContentMetadata(base, declaration.manifest, declaration.state, scratch));
    hal.files = saved;
  }
  hal.files.at("/dictionaries/es/stem.dict")[0] ^= 1;
  EXPECT_FALSE(storage.installContentMetadata(base, declaration.manifest, declaration.state, scratch));
  hal.files = saved;
  declaration.state.phase = TransferPhase::Installing;
  EXPECT_FALSE(storage.installContentMetadata(base, declaration.manifest, declaration.state, scratch));
  EXPECT_EQ(hal.files, saved);
}

TEST_F(HalCourseTransferTest, AuthenticatedDictionaryCommandsInstallHashScopedBundlesIdempotently) {
  for (const char* fixture : {"DictionaryBundle-plain.fixture", "DictionaryBundle-dictzip.fixture"}) {
    SetUp();
    const auto transferScratch = std::span(scratch).subspan(TRANSFER_OFFSET);
    std::ifstream input(std::string(COMPANION_FIXTURE_DIR) + fixture, std::ios::binary);
    bytes = {std::istreambuf_iterator<char>(input), {}};
    ASSERT_FALSE(bytes.empty());
    declaration.manifest.kind = ContentKind::Dictionary;
    declaration.manifest.logicalIdentity.fill(0);
    declaration.manifest.length = bytes.size();
    hashBytes();
    std::string base = "/dictionaries/";
    base.reserve(96);
    constexpr char hex[] = "0123456789abcdef";
    for (const auto byte : declaration.manifest.contentHash) {
      base.push_back(hex[byte >> 4]);
      base.push_back(hex[byte & 15]);
    }
    base += "/dictionary";
    inventory_hal_test::state.enumerateFileMap = true;
    inventory_hal_test::state.directories["/"] = {};
    Transfer transfer(storage, transferScratch);
    auto installer = createHalDictionaryTransferInstaller(transfer, generation, transferScratch);
    ASSERT_TRUE(installer);
    storage.setDictionaryInstaller(installer.get());
    receive(transfer, true, base);
    std::array<uint8_t, 1 + TRANSFER_STATE_SIZE> response{};
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
      const auto before = inventory_hal_test::state.files;
      ASSERT_EQ(
          handleTransfer(transfer, Command::Commit, declaration.state.transaction, declaration.state.owner, response),
          response.size());
      ASSERT_EQ(response[0], static_cast<uint8_t>(TransferResult::Ok));
      ASSERT_EQ(transfer.current()->phase, TransferPhase::Committed);
      if (attempt) {
        EXPECT_EQ(inventory_hal_test::state.files, before);
      }
    }
    auto& files = inventory_hal_test::state.files;
    EXPECT_TRUE(files.contains(base + ".ifo"));
    EXPECT_TRUE(files.contains(base + ".idx"));
    EXPECT_TRUE(files.contains(base + ".syn"));
    EXPECT_TRUE(files.contains(
        base + (std::string_view(fixture).find("dictzip") != std::string_view::npos ? ".dict.dz" : ".dict")));
    for (const auto path : DICTIONARY_INSTALLATION_TEMPORARIES) EXPECT_FALSE(files.contains(path)) << path;
    for (const auto path : DICTIONARY_RETIREMENT_JOURNALS) EXPECT_FALSE(files.contains(path)) << path;
    storage.setDictionaryInstaller(nullptr);
  }
}

TEST_F(HalCourseTransferTest, ConcreteDictionaryPreparationRejectsLowOrFragmentedHeapWithoutMutation) {
  for (unsigned mode = 0; mode < 3; ++mode) {
    SetUp();
    const auto transferScratch = std::span(scratch).subspan(TRANSFER_OFFSET);
    std::ifstream input(std::string(COMPANION_FIXTURE_DIR) + "DictionaryBundle-plain.fixture", std::ios::binary);
    bytes = {std::istreambuf_iterator<char>(input), {}};
    ASSERT_FALSE(bytes.empty());
    declaration.manifest.kind = ContentKind::Dictionary;
    declaration.manifest.logicalIdentity.fill(0);
    declaration.manifest.length = bytes.size();
    hashBytes();
    inventory_hal_test::state.enumerateFileMap = true;
    inventory_hal_test::state.directories["/"] = {};
    constexpr const char* base = "/dictionaries/imported/dictionary";
    Transfer transfer(storage, transferScratch);
    auto installer = createHalDictionaryTransferInstaller(transfer, generation, transferScratch);
    ASSERT_TRUE(installer);
    storage.setDictionaryInstaller(installer.get());
    ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
    ASSERT_EQ(transfer.begin(declaration, base), TransferResult::Ok);
    for (size_t offset = 0; offset < bytes.size();) {
      const auto count = std::min<size_t>(512, bytes.size() - offset);
      ASSERT_EQ(transfer.append(declaration.state.transaction, declaration.state.owner, offset,
                                std::span<const uint8_t>(bytes).subspan(offset, count)),
                TransferResult::Ok);
      offset += count;
    }
    const auto staged = inventory_hal_test::state.files;
    auto& heap = companion_memory_test::internal;
    if (mode == 0) heap.freeBytes = 50 * 1024;
    if (mode == 1) heap.freeBytes = 50 * 1024 + 1;
    if (mode == 2) heap.largestBlockBytes = 1;
    EXPECT_FALSE(
        installer->prepare(base, TRANSFER_STAGE, *transfer.contentManifest(), *transfer.current(), transferScratch))
        << mode;
    EXPECT_EQ(inventory_hal_test::state.files, staged) << mode;
    EXPECT_EQ(transfer.current()->phase, TransferPhase::Receiving);
    storage.setDictionaryInstaller(nullptr);
  }
}

TEST_F(HalCourseTransferTest, ConcreteDictionaryInstallerExtractsPublishesAndRetiresPlainAndCompressedBundles) {
  for (const char* fixture : {"DictionaryBundle-plain.fixture", "DictionaryBundle-dictzip.fixture"}) {
    for (unsigned fault = 0; fault < 6; ++fault) {
      SetUp();
      const auto transferScratch = std::span(scratch).subspan(TRANSFER_OFFSET);
      std::fill(scratch.begin(), scratch.begin() + TRANSFER_OFFSET, 0xa5);
      std::ifstream input(std::string(COMPANION_FIXTURE_DIR) + fixture, std::ios::binary);
      bytes = {std::istreambuf_iterator<char>(input), {}};
      ASSERT_FALSE(bytes.empty());
      declaration.manifest.kind = ContentKind::Dictionary;
      declaration.manifest.logicalIdentity.fill(0);
      declaration.manifest.length = bytes.size();
      hashBytes();
      inventory_hal_test::state.enumerateFileMap = true;
      inventory_hal_test::state.directories["/"] = {};
      constexpr const char* base = "/dictionaries/imported/dictionary";
      Transfer transfer(storage, transferScratch);
      auto installer = createHalDictionaryTransferInstaller(transfer, generation, transferScratch);
      ASSERT_TRUE(installer);
      storage.setDictionaryInstaller(installer.get());
      ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
      ASSERT_EQ(transfer.begin(declaration, base), TransferResult::Ok);
      for (size_t offset = 0; offset < bytes.size();) {
        const auto count = std::min<size_t>(512, bytes.size() - offset);
        ASSERT_EQ(transfer.append(declaration.state.transaction, declaration.state.owner, offset,
                                  std::span<const uint8_t>(bytes).subspan(offset, count)),
                  TransferResult::Ok);
        offset += count;
      }
      if (fault == 1)
        inventory_hal_test::state.failRenameAfterSource = HalZipEntryStage::memberPath(HalZipEntryStage::Member::Info);
      if (fault == 2) inventory_hal_test::state.failSyncPath = DICTIONARY_RETIREMENT_JOURNALS[1];
      if (fault == 3) inventory_hal_test::state.failSyncPath = DICTIONARY_ZIP_AUDIT_JOURNALS[1];
      if (fault == 4) inventory_hal_test::state.failRemoveAfter = true;
      if (fault == 5) {
        auto audit = makeUniqueNoThrow<HalDictionaryZipAudit>(*transfer.current(), *transfer.contentManifest(),
                                                              generation, base, transferScratch);
        ASSERT_TRUE(audit && audit->begin());
        audit.reset();
        for (const auto path :
             {HalZipRangeStorage::PATH, HalZipRangeStorage::NAME_INDEX_PATH, HalZipNameBytesStorage::PATH})
          inventory_hal_test::state.files[path] = {1, 2, 3};
      }
      const auto committed = transfer.commit(declaration.state.transaction, declaration.state.owner);
      if (fault && fault != 5) {
        ASSERT_EQ(committed, fault < 3 ? TransferResult::IoError : TransferResult::Invalid) << fixture;
        ASSERT_EQ(transfer.current()->phase, fault == 1   ? TransferPhase::Installing
                                             : fault == 2 ? TransferPhase::Committed
                                                          : TransferPhase::Receiving);
        inventory_hal_test::state.failRenameAfterSource.clear();
        inventory_hal_test::state.failSyncPath.clear();
        inventory_hal_test::state.failRemoveAfter = false;
        storage.setDictionaryInstaller(nullptr);
        installer.reset();
        installer = createHalDictionaryTransferInstaller(transfer, generation, transferScratch);
        ASSERT_TRUE(installer);
        storage.setDictionaryInstaller(installer.get());
        if (fault <= 2) {
          const auto interrupted = inventory_hal_test::state.files;
          const auto phase = transfer.current()->phase;
          for (unsigned shortage = 0; shortage < 2; ++shortage) {
            companion_memory_test::internal = {1024 * 1024, 1024 * 1024, 1024 * 1024, 1024 * 1024};
            if (shortage == 0) companion_memory_test::internal.freeBytes = 50 * 1024;
            if (shortage == 1) companion_memory_test::internal.largestBlockBytes = 1;
            EXPECT_EQ(transfer.recover(generation), TransferResult::IoError) << fixture << shortage;
            EXPECT_EQ(inventory_hal_test::state.files, interrupted) << fixture << shortage;
            EXPECT_EQ(transfer.current()->phase, phase);
          }
          companion_memory_test::internal = {1024 * 1024, 1024 * 1024, 1024 * 1024, 1024 * 1024};
        }
        ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
        if (fault >= 3) {
          ASSERT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::Ok);
        }
      } else {
        ASSERT_EQ(committed, TransferResult::Ok) << fixture;
      }
      ASSERT_EQ(transfer.current()->phase, TransferPhase::Committed);
      auto& hal = inventory_hal_test::state;
      const std::string definitions =
          std::string(base) + (std::string(fixture).find("dictzip") != std::string::npos ? ".dict.dz" : ".dict");
      EXPECT_TRUE(hal.files.contains(definitions));
      EXPECT_TRUE(hal.files.contains(std::string(base) + ".idx"));
      EXPECT_TRUE(hal.files.contains(std::string(base) + ".ifo"));
      EXPECT_TRUE(hal.files.contains(std::string(base) + ".syn"));
      for (const auto path : DICTIONARY_INSTALLATION_TEMPORARIES) EXPECT_FALSE(hal.files.contains(path)) << path;
      for (const auto path : DICTIONARY_RETIREMENT_JOURNALS) EXPECT_FALSE(hal.files.contains(path)) << path;
      const auto installed = hal.files;
      ASSERT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::Ok);
      EXPECT_EQ(hal.files, installed);
      storage.setDictionaryInstaller(nullptr);
      installer.reset();
      Transfer rebooted(storage, transferScratch);
      auto resumed = createHalDictionaryTransferInstaller(rebooted, generation, transferScratch);
      ASSERT_TRUE(resumed);
      storage.setDictionaryInstaller(resumed.get());
      ASSERT_EQ(rebooted.recover(generation), TransferResult::Ok);
      ASSERT_EQ(rebooted.current()->phase, TransferPhase::Committed);
      EXPECT_EQ(hal.files, installed);
      EXPECT_TRUE(
          std::all_of(scratch.begin(), scratch.begin() + TRANSFER_OFFSET, [](uint8_t byte) { return byte == 0xa5; }));
      storage.setDictionaryInstaller(nullptr);
    }
  }
}

TEST_F(HalCourseTransferTest, ConcreteDictionaryAbortPreservesInstalledContentAndResumesOwnedCleanup) {
  for (unsigned mode = 0; mode < 8; ++mode) {
    SetUp();
    const auto transferScratch = std::span(scratch).subspan(TRANSFER_OFFSET);
    std::fill(scratch.begin(), scratch.begin() + TRANSFER_OFFSET, 0xa5);
    std::ifstream input(std::string(COMPANION_FIXTURE_DIR) + "DictionaryBundle-plain.fixture", std::ios::binary);
    bytes = {std::istreambuf_iterator<char>(input), {}};
    ASSERT_FALSE(bytes.empty());
    declaration.manifest.kind = ContentKind::Dictionary;
    declaration.manifest.logicalIdentity.fill(0);
    declaration.manifest.length = bytes.size();
    hashBytes();
    auto& hal = inventory_hal_test::state;
    hal.enumerateFileMap = true;
    hal.directories["/"] = {};
    hal.files["/dictionaries/existing/dictionary.dict"] = {8, 7, 6};
    const auto existing = hal.files.at("/dictionaries/existing/dictionary.dict");
    constexpr const char* base = "/dictionaries/imported/dictionary";
    Transfer transfer(storage, transferScratch);
    auto installer = createHalDictionaryTransferInstaller(transfer, generation, transferScratch);
    ASSERT_TRUE(installer);
    storage.setDictionaryInstaller(installer.get());
    ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
    ASSERT_EQ(transfer.begin(declaration, base), TransferResult::Ok);
    const auto count = mode == 0 ? std::min<size_t>(16, bytes.size()) : bytes.size();
    for (size_t offset = 0; offset < count;) {
      const auto chunk = std::min<size_t>(512, count - offset);
      ASSERT_EQ(transfer.append(declaration.state.transaction, declaration.state.owner, offset,
                                std::span<const uint8_t>(bytes).subspan(offset, chunk)),
                TransferResult::Ok);
      offset += chunk;
    }
    if (mode && mode != 7) {
      hal.failSyncPath = DICTIONARY_INSTALLATION_JOURNALS[0];
      ASSERT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::Invalid);
      ASSERT_EQ(transfer.current()->phase, TransferPhase::Receiving);
      hal.failSyncPath.clear();
      ASSERT_TRUE(hal.files.contains(DICTIONARY_EXTRACTION_JOURNALS[0]));
      ASSERT_TRUE(hal.files.contains(DICTIONARY_INSTALLATION_JOURNALS[0]));
    }
    if (mode == 7) {
      hal.failSyncPath = DICTIONARY_ZIP_AUDIT_JOURNALS[0];
      ASSERT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::Invalid);
      hal.failSyncPath.clear();
      ASSERT_TRUE(hal.files.contains(DICTIONARY_ZIP_AUDIT_JOURNALS[0]));
      ASSERT_FALSE(hal.files.contains(DICTIONARY_EXTRACTION_JOURNALS[0]));
    }
    if (mode == 6) {
      auto audit = makeUniqueNoThrow<HalDictionaryZipAudit>(*transfer.current(), *transfer.contentManifest(),
                                                            generation, base, transferScratch);
      ASSERT_TRUE(audit && audit->begin());
      audit.reset();
      for (const auto path :
           {HalZipRangeStorage::PATH, HalZipRangeStorage::NAME_INDEX_PATH, HalZipNameBytesStorage::PATH})
        hal.files[path] = {4, 3};
    }
    if (mode == 2) {
      // The incoming stage was removed before a reset; fail the first owned member removal.
      hal.files.erase(TRANSFER_STAGE);
      hal.failRemoveAfter = true;
    }
    const auto secondReceipt =
        mode && mode != 7 ? hal.files.at(DICTIONARY_EXTRACTION_JOURNALS[1]) : std::vector<uint8_t>{};
    if (mode == 3) hal.files.at(HalZipEntryStage::memberPath(HalZipEntryStage::Member::Info))[0] ^= 1;
    if (mode == 4) {
      DictionaryExtractionReceipt foreign;
      ASSERT_TRUE(decodeDictionaryExtractionReceipt(secondReceipt, foreign));
      foreign.archiveHash[0] ^= 0x80;
      ASSERT_EQ(encodeDictionaryExtractionReceipt(foreign, transferScratch), DICTIONARY_EXTRACTION_RECEIPT_SIZE);
      hal.files[DICTIONARY_EXTRACTION_JOURNALS[1]] = {transferScratch.begin(),
                                                      transferScratch.begin() + DICTIONARY_EXTRACTION_RECEIPT_SIZE};
    }
    constexpr const char* unownedIndex = "/.crosspoint/companion/zip-ranges-next";
    if (mode == 5) hal.files[unownedIndex] = {4, 3};
    const auto receipts = mode && mode != 7 ? hal.files.at(DICTIONARY_EXTRACTION_JOURNALS[0]) : std::vector<uint8_t>{};
    const auto aborted = transfer.abort(declaration.state.transaction, declaration.state.owner);
    ASSERT_EQ(transfer.current()->phase, TransferPhase::Aborted);
    EXPECT_EQ(aborted, mode >= 2 && mode <= 5 ? TransferResult::IoError : TransferResult::Ok);
    EXPECT_EQ(hal.files.at("/dictionaries/existing/dictionary.dict"), existing);
    if (mode >= 2 && mode <= 5) {
      EXPECT_EQ(hal.files.at(DICTIONARY_EXTRACTION_JOURNALS[0]), receipts);
      hal.failRemoveAfter = false;
      if (mode == 3) {
        EXPECT_TRUE(hal.files.contains(HalZipEntryStage::memberPath(HalZipEntryStage::Member::Definitions)));
        hal.files.at(HalZipEntryStage::memberPath(HalZipEntryStage::Member::Info))[0] ^= 1;
      }
    }
    if (mode == 4) {
      EXPECT_TRUE(hal.files.contains(HalZipEntryStage::memberPath(HalZipEntryStage::Member::Definitions)));
      hal.files[DICTIONARY_EXTRACTION_JOURNALS[1]] = secondReceipt;
    }
    if (mode == 5) {
      EXPECT_EQ(hal.files.at(unownedIndex), (std::vector<uint8_t>{4, 3}));
      EXPECT_TRUE(hal.files.contains(HalZipEntryStage::memberPath(HalZipEntryStage::Member::Definitions)));
      hal.files.erase(unownedIndex);
    }
    storage.setDictionaryInstaller(nullptr);
    installer.reset();
    Transfer reopened(storage, transferScratch);
    auto recovered = createHalDictionaryTransferInstaller(reopened, generation, transferScratch);
    ASSERT_TRUE(recovered);
    storage.setDictionaryInstaller(recovered.get());
    ASSERT_EQ(reopened.recover(generation), TransferResult::Ok) << mode;
    ASSERT_EQ(reopened.current()->phase, TransferPhase::Aborted);
    for (const auto path : DICTIONARY_INSTALLATION_TEMPORARIES) EXPECT_FALSE(hal.files.contains(path)) << path;
    const auto cleaned = hal.files;
    ASSERT_EQ(reopened.abort(declaration.state.transaction, declaration.state.owner), TransferResult::Ok);
    EXPECT_EQ(hal.files, cleaned);
    EXPECT_EQ(hal.files.at("/dictionaries/existing/dictionary.dict"), existing);
    EXPECT_TRUE(
        std::all_of(scratch.begin(), scratch.begin() + TRANSFER_OFFSET, [](uint8_t byte) { return byte == 0xa5; }));
    storage.setDictionaryInstaller(nullptr);
  }
}

TEST_F(HalCourseTransferTest, InitialMigrationAdmissionRequiresOwnerPackJournalAndReviewedSources) {
  for (const bool preferenceOnly : {false, true}) {
    SCOPED_TRACE(preferenceOnly);
    auto& state = inventory_hal_test::state;
    state = {};
    state.enumerateFileMap = true;
    ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
    state.files[ACTIVE_COURSE_PATH] = bytes;
    std::array<uint8_t, COURSE_BINDING_SIZE> binding{};
    ASSERT_EQ(encodeCourseBinding(declaration.manifest, binding), binding.size());
    state.files[COURSE_BINDING_PATH] = {binding.begin(), binding.end()};
    TintaMigrationAdmission admission;
    admission.reader.fill(1);
    admission.merge.generation = generation;
    admission.course = declaration.manifest.logicalIdentity;
    admission.resource = declaration.manifest.contentHash;
    admission.backupTransaction.fill(4);
    admission.merge.owner.fill(5);
    admission.merge.transaction.fill(6);
    admission.merge.merged = {1, 1024, {}};
    admission.merge.merged.frontier.fill(8);
    {
      auto audit = makeUniqueNoThrow<HalJournalCausalAuditSession>();
      ASSERT_TRUE(audit);
      ASSERT_TRUE(audit->run(&admission.merge.previous.frontier));
      admission.merge.previous.count = audit->recordCount();
      admission.merge.previous.recordSize = audit->recordSize();
    }
    std::array<char, COURSE_STATE_DIRECTORY_SIZE> root{};
    ASSERT_TRUE(courseStateDirectory(admission.course, root));
    ASSERT_TRUE(Storage.ensureDirectoryExists(root.data()));
    for (const auto* name : {"reviews.log", "items.bin", "profile.bin"})
      state.files[std::string(root.data()) + "/" + name] = std::vector<uint8_t>(80, 5);
    HalTintaJournalStorage hashing;
    {
      auto backup = makeUniqueNoThrow<HalLegacyTintaBackupSession>();
      ASSERT_TRUE(backup);
      ASSERT_TRUE(
          backup->captureCourse(admission.reader, generation, admission.course, admission.backupTransaction, true));
      std::array<uint8_t, LEGACY_TINTA_BACKUP_MANIFEST_SIZE> manifest{};
      ASSERT_TRUE(backup->readManifest(manifest));
      ASSERT_TRUE(hashing.digest(manifest, admission.backupManifest));
    }
    IdentityState identity{admission.reader, generation, 1};
    const auto attempt = [&](const TintaMigrationAdmission& value, const Identity& owner) {
      auto context = makeUniqueNoThrow<HalTintaJournalMergeCommitContext>();
      EXPECT_TRUE(context);
      return context && context->admitMigration(value, identity, owner);
    };
    Identity foreign{};
    foreign.fill(99);
    EXPECT_FALSE(attempt(admission, foreign));
    auto changed = admission;
    changed.reader = foreign;
    EXPECT_FALSE(attempt(changed, admission.merge.owner));
    changed = admission;
    changed.merge.generation = foreign;
    EXPECT_FALSE(attempt(changed, admission.merge.owner));
    changed = admission;
    changed.backupManifest[0] ^= 1;
    EXPECT_FALSE(attempt(changed, admission.merge.owner));
    changed = admission;
    changed.resource[0] ^= 1;
    EXPECT_FALSE(attempt(changed, admission.merge.owner));
    changed = admission;
    changed.merge.previous.frontier[0] ^= 1;
    EXPECT_FALSE(attempt(changed, admission.merge.owner));
    const auto profilePath = std::string(root.data()) + "/profile.bin";
    state.files[profilePath][0] ^= 1;
    EXPECT_FALSE(attempt(admission, admission.merge.owner));
    state.files[profilePath][0] ^= 1;
    ASSERT_TRUE(attempt(admission, admission.merge.owner));
    ASSERT_TRUE(attempt(admission, admission.merge.owner));
    auto records = makeUniqueNoThrow<HalTintaMigrationAdmissions>(hashing);
    ASSERT_TRUE(records);
    TintaMigrationAdmission loaded;
    ASSERT_EQ(records->load(admission.course, admission.merge, loaded), TintaMigrationAdmissionResult::Ok);
    EXPECT_EQ(loaded, admission);
    std::array<uint8_t, TINTA_MIGRATION_ADMISSION_SIZE> command{};
    ASSERT_TRUE(encodeTintaMigrationAdmission(admission, command));
    std::array<uint8_t, 24> response{};
    auto commandContext = makeUniqueNoThrow<HalTintaJournalMergeCommitContext>();
    ASSERT_TRUE(commandContext);
    const auto beforeForeignOwner = state.files;
    ASSERT_EQ(commandContext->migrationAdmissionReply(command, identity, foreign, response), response.size());
    EXPECT_EQ(response[1], static_cast<uint8_t>(TintaJournalResult::Conflict));
    EXPECT_EQ(state.files, beforeForeignOwner);
    ASSERT_EQ(commandContext->migrationAdmissionReply(command, identity, admission.merge.owner, response),
              response.size());
    EXPECT_EQ(response[1], static_cast<uint8_t>(TintaJournalResult::Ok));
    EXPECT_TRUE(
        std::equal(admission.merge.transaction.begin(), admission.merge.transaction.end(), response.begin() + 4));
    const auto untouchedReply = response;
    command[255] ^= 1;
    EXPECT_EQ(commandContext->migrationAdmissionReply(command, identity, admission.merge.owner, response), 0u);
    EXPECT_EQ(response, untouchedReply);
    command[255] ^= 1;
    std::array<uint8_t, JOURNAL_MERGE_INTENT_SIZE> mergeDeclaration{};
    ASSERT_TRUE(encodeJournalMergeIntent(admission.merge, mergeDeclaration));
    EXPECT_FALSE(commandContext->prepareMigration(mergeDeclaration, identity, foreign));
    ASSERT_TRUE(commandContext->prepareMigration(mergeDeclaration, identity, admission.merge.owner));
    state.files[profilePath][0] ^= 1;
    EXPECT_FALSE(commandContext->prepareMigration(mergeDeclaration, identity, admission.merge.owner));
    EXPECT_EQ(commandContext->catalog(), nullptr);
    state.files[profilePath][0] ^= 1;
    commandContext.reset();
    HalInventoryIndexStorage packStorage;
    ASSERT_TRUE(packStorage.open(ACTIVE_COURSE_PATH));
    StoredCourseSource source(packStorage);
    ASSERT_TRUE(source.attach());
    tinta::core::pack::Pack pack;
    ASSERT_EQ(validateCourseCandidate(pack, source, scratch), CourseValidationResult::Ok);
    TintaPackSubjectCatalog catalog(pack, source);
    ASSERT_TRUE(catalog.prepare(scratch));
    const auto uid = pack.uidAt(0);
    {
      HalTintaJournalStorage journalStorage;
      std::array<uint8_t, 1024> journalScratch{};
      TintaJournal journal(journalStorage, journalScratch);
      ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
      TintaBody body;
      body.kind = EventKind::Star;
      body.course = admission.course;
      body.uid = uid;
      body.enabled = true;
      std::array<uint8_t, MAX_TINTA_BODY_SIZE> encoded{};
      size_t size = encodeTintaBody(body, encoded);
      if (preferenceOnly) {
        const std::array<uint8_t, 8> preference{1, 4, 32, 1, 20, 0, 0, 0};
        std::copy(preference.begin(), preference.end(), encoded.begin());
        size = preference.size();
      }
      SyncEvent event;
      event.identity = {admission.merge.owner, 1, 1};
      event.storageGeneration = foreign;
      event.kind = preferenceOnly ? EventKind::Preference : body.kind;
      event.studyDay = preferenceOnly ? 0 : 12;
      event.resource = preferenceOnly ? PREFERENCE_SCOPE : admission.resource;
      if (!preferenceOnly) event.resource.fill(19);
      ASSERT_TRUE(journalStorage.digest(std::span(encoded).first(size), event.bodyHash));
      ASSERT_EQ(journal.append(event, std::span(encoded).first(size)), TintaJournalResult::Ok);
    }
    {
      auto audit = makeUniqueNoThrow<HalJournalCausalAuditSession>();
      ASSERT_TRUE(audit);
      ASSERT_TRUE(audit->run(&admission.merge.merged.frontier));
      admission.merge.merged.count = audit->recordCount();
      admission.merge.merged.recordSize = audit->recordSize();
    }
    auto initial = makeUniqueNoThrow<HalTintaInitialMigrationReconciliation>();
    ASSERT_TRUE(initial);
    EXPECT_FALSE(initial->run(admission.course, identity, admission.resource, admission.merge, catalog));
    ASSERT_EQ(records->persist(admission), TintaMigrationAdmissionResult::Ok);
    state.files[profilePath][0] ^= 1;
    EXPECT_FALSE(initial->run(admission.course, identity, admission.resource, admission.merge, catalog));
    state.files[profilePath][0] ^= 1;
    initial.reset();
    class MigrationIdentities final : public IdentityStorage {
     public:
      explicit MigrationIdentities(const IdentityState& identity) {
        binding[0] = 'L';
        binding[1] = 'C';
        binding[2] = 'I';
        binding[3] = 1;
        std::copy(identity.device.begin(), identity.device.end(), binding.begin() + 4);
        std::fill_n(binding.begin() + 20, 16, 2);
        std::fill_n(binding.begin() + 36, 16, 3);
        std::copy(identity.storageGeneration.begin(), identity.storageGeneration.end(), binding.begin() + 52);
        tinta_body_detail::write(binding, 68, identity.eventEpoch, 8);
        tinta_body_detail::write(binding, 76, binary_record::crc32(binding.data(), 76), 4);
      }
      bool hardwareIdentity(Identity& output) override {
        std::copy_n(binding.begin() + 4, 16, output.begin());
        return true;
      }
      bool cardIdentity(Identity& output) override {
        output.fill(2);
        return true;
      }
      IdentityRead readBinding(std::span<uint8_t> output) override {
        std::copy(binding.begin(), binding.end(), output.begin());
        return IdentityRead::Present;
      }
      bool writeBinding(std::span<const uint8_t> input) override {
        std::copy(input.begin(), input.end(), binding.begin());
        return true;
      }
      IdentityRead readMarker(Identity& output) override {
        output.fill(3);
        return IdentityRead::Present;
      }
      bool createMarker(const Identity&) override { return false; }
      bool randomIdentity(Identity& output) override {
        output.fill(9);
        return true;
      }
      std::array<uint8_t, IDENTITY_RECORD_SIZE> binding{};
    } identities(identity);
    HalJournalMergeRecordStore mergeReceipt(JournalMergeRecord::Receipt);
    ASSERT_TRUE(mergeReceipt.persist(admission.merge));
    std::array<char, COURSE_STATE_PATH_SIZE> itemsPath{};
    ASSERT_TRUE(tintaDerivedFilePath(admission.course, TintaDerivedFile::Items, TintaDerivedRole::Active, itemsPath));
    state.failRenameAfterSource = itemsPath.data();
    {
      auto reconciliation = makeUniqueNoThrow<HalTintaMergedJournalReconciliation>(admission.course, identities);
      ASSERT_TRUE(reconciliation);
      EXPECT_FALSE(reconciliation->run());
    }
    state.failRenameAfterSource.clear();
    HalTintaDerivedRecordReader receiptReader(admission.course, scratch);
    std::array<uint8_t, TINTA_DERIVED_MANIFEST_SIZE> receiptBytes{};
    ASSERT_EQ(receiptReader.load(TintaDerivedRecord::Intent, receiptBytes), TintaDerivedRecordLoad::Loaded);
    {
      auto recovery = makeUniqueNoThrow<HalTintaDerivedStartupRecovery>(admission.course, identities);
      ASSERT_TRUE(recovery);
      ASSERT_TRUE(recovery->run());
    }
    ASSERT_EQ(receiptReader.load(TintaDerivedRecord::Receipt, receiptBytes), TintaDerivedRecordLoad::Loaded);
    TintaDerivedManifestView receipt;
    ASSERT_TRUE(receipt.decode(receiptBytes));
    EXPECT_TRUE(receipt.matches(admission.course, generation, admission.resource, admission.merge.merged.frontier));
    EXPECT_EQ(receipt.revision(), 1u);
    EXPECT_EQ(receipt.studyDay(), preferenceOnly ? 0u : 12u);
    EXPECT_EQ(state.files.at(profilePath), std::vector<uint8_t>(80, 5));
    if (preferenceOnly) {
      EXPECT_EQ(state.files.at(itemsPath.data()).size(), 1024u);
    } else {
      tinta::core::ItemState item;
      ASSERT_TRUE(tinta::core::ItemState::decode(state.files.at(itemsPath.data()).data() + 1024, item));
      EXPECT_EQ(item.uid, uid);
      EXPECT_TRUE(item.flags & tinta::core::item_flag::kStarred);
    }
    auto preserved = makeUniqueNoThrow<HalLegacyTintaBackupSession>();
    ASSERT_TRUE(preserved);
    ASSERT_TRUE(preserved->prepareCourse(admission.reader, generation, admission.course, admission.backupTransaction,
                                         true, true));
    ASSERT_TRUE(preserved->recover());
    std::array<uint8_t, LEGACY_TINTA_BACKUP_MANIFEST_SIZE> backupManifest{};
    ASSERT_TRUE(preserved->readManifest(backupManifest));
    Digest backupHash{};
    ASSERT_TRUE(hashing.digest(backupManifest, backupHash));
    EXPECT_EQ(backupHash, admission.backupManifest);
    auto reconciliation = makeUniqueNoThrow<HalTintaMergedJournalReconciliation>(admission.course, identities);
    ASSERT_TRUE(reconciliation);
    ASSERT_TRUE(reconciliation->run());
    const auto reconciledFiles = state.files;
    const auto reconciledIdentity = identities.binding;
    ASSERT_TRUE(reconciliation->run());
    EXPECT_EQ(state.files, reconciledFiles);
    EXPECT_EQ(identities.binding, reconciledIdentity);
  }
}
