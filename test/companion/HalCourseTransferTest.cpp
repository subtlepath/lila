#include <Memory.h>
#include <gtest/gtest.h>
#include <openssl/sha.h>

#include <fstream>
#include <iterator>

#include "HalStorage.h"
#include "lib/Companion/CompanionCourseBaselineJournalSnapshot.h"
#include "lib/Companion/CompanionCourseBaselineTransfer.h"
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
#include "lib/hal/HalCourseBaselineArchiveSession.h"
#include "lib/hal/HalCourseBaselineImportBegin.h"
#include "lib/hal/HalCourseBaselineImportCommit.h"
#include "lib/hal/HalCourseBaselineImportConsentStore.h"
#include "lib/hal/HalCourseBaselineImportPreparation.h"
#include "lib/hal/HalCourseBaselineImportSession.h"
#include "lib/hal/HalCourseBaselineLearnerInspection.h"
#include "lib/hal/HalCourseBaselineNativeInstaller.h"
#include "lib/hal/HalCourseBaselinePublicationStore.h"
#include "lib/hal/HalCourseBaselineRecovery.h"
#include "lib/hal/HalCourseBaselineReplayReceipt.h"
#include "lib/hal/HalCourseBaselineReplaySession.h"
#include "lib/hal/HalCourseBaselineReviewBackup.h"
#include "lib/hal/HalCourseBaselineReviewCapture.h"
#include "lib/hal/HalCourseBaselineReviewPage.h"
#include "lib/hal/HalCourseBaselineReviewRequest.h"
#include "lib/hal/HalCourseBaselineReviewStore.h"
#include "lib/hal/HalCoursePackArchive.h"
#include "lib/hal/HalCoursePackHistory.h"
#include "lib/hal/HalCoursePackHistoryValidator.h"
#include "lib/hal/HalCourseRemovalMetadata.h"
#include "lib/hal/HalCourseRemovalNativeOwner.h"
#include "lib/hal/HalCourseRemovalPreparation.h"
#include "lib/hal/HalCourseRemovalRecovery.h"
#include "lib/hal/HalHistoricalCourseBaseline.h"
#include "lib/hal/HalHistoricalCourseHistory.h"
#include "lib/hal/HalRemovedCourseBaseline.h"
#include "lib/hal/HalTintaReplayItemCorrespondence.h"
#include "lib/hal/HalTintaReplayItemExport.h"
#include "lib/hal/HalTransferStorage.h"
#include "lib/hal/HalUnboundCourseBoundLessonMapping.h"
#include "lib/hal/HalUnboundCourseBoundReadingMapping.h"
#include "lib/hal/HalUnboundCourseDayInspection.h"
#include "lib/hal/HalUnboundCourseItemInspection.h"
#include "lib/hal/HalUnboundCourseLearnerInspection.h"
#include "lib/hal/HalUnboundCourseMarkInspection.h"
#include "lib/hal/HalUnboundCourseMigrationIntentStore.h"
#include "lib/hal/HalUnboundCoursePackVerification.h"
#include "lib/hal/HalUnboundCourseProfileInspection.h"
#include "lib/hal/HalUnboundCourseReviewInspection.h"
#include "lib/hal/HalUnboundCourseReviewedFile.h"
#include "lib/hal/HalUnboundCourseSessionInspection.h"
#include "platform/StateFiles.h"
namespace tinta::platform {
void log(const char*, ...) {}
}  // namespace tinta::platform
using namespace companion;

namespace {
struct BaselineRecoveryIdentities final : IdentityStorage {
  Identity device{}, card{}, marker{};
  std::array<uint8_t, IDENTITY_RECORD_SIZE> binding{};
  unsigned writes = 0;
  bool fail = false;
  BaselineRecoveryIdentities(const Identity& reader, const Identity& generation) : device(reader) {
    card[0] = 71;
    marker[0] = 72;
    std::memcpy(binding.data(), "LCI\1", 4);
    std::copy(device.begin(), device.end(), binding.begin() + 4);
    std::copy(card.begin(), card.end(), binding.begin() + 20);
    std::copy(marker.begin(), marker.end(), binding.begin() + 36);
    std::copy(generation.begin(), generation.end(), binding.begin() + 52);
    binary_record::putU32(binding.data() + 68, 1);
    binary_record::putU32(binding.data() + 76, binary_record::crc32(binding.data(), 76));
  }
  bool hardwareIdentity(Identity& output) override {
    output = device;
    return !fail;
  }
  bool cardIdentity(Identity& output) override {
    output = card;
    return !fail;
  }
  IdentityRead readBinding(std::span<uint8_t> output) override {
    if (fail) return IdentityRead::Error;
    std::copy(binding.begin(), binding.end(), output.begin());
    return IdentityRead::Present;
  }
  IdentityRead readMarker(Identity& output) override {
    output = marker;
    return fail ? IdentityRead::Error : IdentityRead::Present;
  }
  bool writeBinding(std::span<const uint8_t>) override {
    ++writes;
    return false;
  }
  bool createMarker(const Identity&) override {
    ++writes;
    return false;
  }
  bool randomIdentity(Identity&) override {
    ++writes;
    return false;
  }
};
struct BaselineRecoveryPairings final : PairingsStorage {
  std::array<uint8_t, PAIRINGS_RECORD_SIZE> data{};
  bool exists = false, fail = false;
  unsigned writes = 0;
  PairingsRead read(std::span<uint8_t> output) override {
    if (fail) return PairingsRead::Error;
    if (!exists) return PairingsRead::Missing;
    std::copy(data.begin(), data.end(), output.begin());
    return PairingsRead::Present;
  }
  bool write(std::span<const uint8_t> input) override {
    ++writes;
    std::copy(input.begin(), input.end(), data.begin());
    exists = true;
    return true;
  }
};
}  // namespace

class HalCourseTransferTest : public testing::Test {
 protected:
  std::vector<uint8_t> bytes;
  std::array<uint8_t, SESSION_WORKSPACE_SIZE> scratch{};
  Identity generation{};
  TransferDeclaration declaration;
  HalTransferStorage storage;
  void SetUp() override {
    inventory_hal_test::state = {};
    inventory_hal_test::state.enumerateFileMap = true;
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
  void removeInstalledCourse(uint8_t transaction = 71) {
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
    plan.request.transaction.fill(transaction);
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
  void prepareBaselineReview(std::vector<uint8_t>& encoded, Digest& hash, Identity& reader,
                             std::span<const uint8_t> learnerItems = {}) {
    inventory_hal_test::state.files["/tinta/items.bin"] =
        learnerItems.empty() ? std::vector<uint8_t>{17}
                             : std::vector<uint8_t>{learnerItems.begin(), learnerItems.end()};
    removeInstalledCourse();
    ASSERT_FALSE(HasFatalFailure());
    reader[0] = 51;
    auto capture = makeUniqueNoThrow<HalCourseBaselineReviewCapture>(scratch, [](void*) { return true; }, nullptr);
    ASSERT_TRUE(capture);
    ASSERT_EQ(capture->capture(reader, generation, declaration.manifest.logicalIdentity),
              CourseBaselineReviewResult::Ok);
    ASSERT_NE(capture->hash(), nullptr);
    hash = *capture->hash();
    encoded.assign(capture->bytes().begin(), capture->bytes().end());
  }
  std::string prepareUnboundBackupReview(Digest& hash, Identity& reader) {
    auto& hal = inventory_hal_test::state;
    hal.directories["/tinta"] = {};
    hal.directories[TRANSFER_DIRECTORY] = {};
    hal.files["/tinta/ITEMS.BIN"] = std::vector<uint8_t>(12000, 17);
    hal.files["/tinta/usage.bin"] = {23};
    reader[0] = 51;
    auto capture = makeUniqueNoThrow<HalCourseBaselineReviewCapture>(scratch, [](void*) { return true; }, nullptr);
    if (!capture ||
        capture->capture(reader, generation, declaration.manifest.logicalIdentity) != CourseBaselineReviewResult::Ok)
      return {};
    hash = *capture->hash();
    const std::vector<uint8_t> encoded(capture->bytes().begin(), capture->bytes().end());
    capture.reset();
    auto store = makeUniqueNoThrow<HalCourseBaselineReviewStore>(
        std::span(scratch).subspan(COURSE_BASELINE_REVIEW_MAX_SIZE), [](void*) { return true; }, nullptr);
    if (!store || store->publish(encoded, hash) != CourseBaselineReviewStoreResult::Ok) return {};
    std::string prefix = store->path();
    prefix.replace(prefix.find("course-review-"), strlen("course-review-"), "course-review-state-");
    return prefix;
  }
  void prepareBaselineApproval(CourseBaselineImportRequest& request, Identity& reader, std::string& consentPath,
                               std::span<const uint8_t> learnerItems = {}) {
    std::vector<uint8_t> encoded;
    Digest hash{};
    prepareBaselineReview(encoded, hash, reader, learnerItems);
    ASSERT_FALSE(HasFatalFailure());
    auto& hal = inventory_hal_test::state;
    for (auto iterator = hal.files.begin(); iterator != hal.files.end();) {
      if (iterator->first.starts_with("/.crosspoint/companion/removal-done-"))
        iterator = hal.files.erase(iterator);
      else
        ++iterator;
    }
    auto store = makeUniqueNoThrow<HalCourseBaselineReviewStore>(
        std::span(scratch).subspan(COURSE_BASELINE_REVIEW_MAX_SIZE), [](void*) { return true; }, nullptr);
    ASSERT_TRUE(store);
    ASSERT_EQ(store->publish(encoded, hash), CourseBaselineReviewStoreResult::Ok);
    request = {generation, declaration.state.owner, declaration.state.transaction, declaration.manifest, hash};
    consentPath = "/.crosspoint/companion/course-baseline-";
    static constexpr char HEX_DIGITS[] = "0123456789abcdef";
    for (const auto byte : request.transaction) {
      consentPath += HEX_DIGITS[byte >> 4];
      consentPath += HEX_DIGITS[byte & 15];
    }
    consentPath += ".consent";
  }
  std::vector<uint8_t> reviewedJournalCopies(Digest& hash) {
    std::ifstream fixture(std::string(COMPANION_FIXTURE_DIR) + "CourseBaselineReview-v1.fixture", std::ios::binary);
    std::vector<uint8_t> review{std::istreambuf_iterator<char>(fixture), std::istreambuf_iterator<char>()};
    if (review.size() != 608) return {};
    review[5] = 1;
    std::copy(declaration.manifest.logicalIdentity.begin(), declaration.manifest.logicalIdentity.end(),
              review.begin() + 40);
    static constexpr const char* PATHS[] = {TINTA_JOURNAL_EVENTS, TINTA_JOURNAL_HEADER_A, TINTA_JOURNAL_HEADER_B};
    auto& files = inventory_hal_test::state.files;
    for (unsigned index = 0; index < 3; ++index) {
      const auto found = files.find(PATHS[index]);
      auto entry = std::span(review).subspan(60 + (index + 1) * 68, 68);
      entry[1] = found != files.end();
      if (found == files.end()) continue;
      course_review_detail::number(entry, 28, found->second.size(), 8);
      SHA256(found->second.data(), found->second.size(), entry.data() + 36);
    }
    course_review_detail::number(review, review.size() - 4, binary_record::crc32(review.data(), review.size() - 4), 4);
    SHA256(review.data(), review.size(), hash.data());
    std::string prefix = "/.crosspoint/companion/course-review-state-";
    static constexpr char DIGITS[] = "0123456789abcdef";
    for (const auto byte : hash) {
      prefix += DIGITS[byte >> 4];
      prefix += DIGITS[byte & 15];
    }
    for (unsigned index = 0; index < 3; ++index) {
      const auto found = files.find(PATHS[index]);
      if (found != files.end()) files[prefix + "-0" + DIGITS[index + 1]] = found->second;
    }
    return review;
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
      hal.enumerateFileMap = true;
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
    hal.enumerateFileMap = true;
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
    hal.enumerateFileMap = true;
    hal.files[ACTIVE_COURSE_PATH] = original;
    hal.files[COURSE_BINDING_PATH] = {binding.begin(), binding.end()};
    hal.files["/tinta/items.bin"] = {17};
    {
      Transfer transfer(storage, scratch);
      receive(transfer);
      ASSERT_TRUE(storage.validateContent(ACTIVE_COURSE_PATH, TRANSFER_STAGE, declaration.manifest, *transfer.current(),
                                          scratch));
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
    hal.enumerateFileMap = true;
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
TEST_F(HalCourseTransferTest, CourseContextVerifiesLivePackAndRefusesUnfinishedPublication) {
  auto& hal = inventory_hal_test::state;
  ASSERT_TRUE(storage.prepare());
  ASSERT_TRUE(Storage.ensureDirectoryExists("/tinta"));
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  std::array<uint8_t, COURSE_BINDING_SIZE> binding{};
  ASSERT_EQ(encodeCourseBinding(declaration.manifest, binding), binding.size());
  hal.files[COURSE_BINDING_PATH] = {binding.begin(), binding.end()};
  bool allowed = true;
  auto permitted = [](void* context) { return *static_cast<bool*>(context); };
  HalRemovedCourseBaseline owner(generation, scratch, permitted, &allowed);
  ContentManifest output;
  auto source = CourseContextSource::None;
  const auto before = hal.files;
  ASSERT_EQ(owner.inspectCurrentCourse(output, source), CourseContextResult::Ok);
  EXPECT_EQ(output, declaration.manifest);
  EXPECT_EQ(source, CourseContextSource::Live);
  EXPECT_EQ(hal.files, before);
  EXPECT_FALSE(owner.path());
  const auto saved = output;
  allowed = false;
  EXPECT_EQ(owner.inspectCurrentCourse(output, source), CourseContextResult::IoError);
  EXPECT_EQ(output, saved);
  EXPECT_EQ(source, CourseContextSource::Live);
  EXPECT_EQ(hal.files, before);
  allowed = true;
  hal.files[COURSE_BINDING_STAGE] = {1};
  const auto staged = hal.files;
  EXPECT_EQ(owner.inspectCurrentCourse(output, source), CourseContextResult::Busy);
  EXPECT_EQ(hal.files, staged);
  EXPECT_EQ(output, saved);
  hal.files = before;
  hal.files.at(ACTIVE_COURSE_PATH)[0] ^= 1;
  const auto corrupt = hal.files;
  EXPECT_EQ(owner.inspectCurrentCourse(output, source), CourseContextResult::Corrupt);
  EXPECT_EQ(hal.files, corrupt);
  EXPECT_EQ(output, saved);
}
TEST_F(HalCourseTransferTest, CourseContextDistinguishesMissingPackFromUnboundLegacyPack) {
  ASSERT_TRUE(storage.prepare());
  ASSERT_TRUE(Storage.ensureDirectoryExists("/tinta"));
  auto& hal = inventory_hal_test::state;
  HalRemovedCourseBaseline owner(generation, scratch, [](void*) { return true; }, nullptr);
  auto output = declaration.manifest;
  auto source = CourseContextSource::Live;
  const auto before = hal.files;
  EXPECT_EQ(owner.inspectCurrentCourse(output, source), CourseContextResult::Missing);
  EXPECT_EQ(hal.files, before);
  EXPECT_EQ(output, declaration.manifest);
  EXPECT_EQ(source, CourseContextSource::Live);
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  const auto legacy = hal.files;
  EXPECT_EQ(owner.inspectCurrentCourse(output, source), CourseContextResult::Unsupported);
  EXPECT_EQ(hal.files, legacy);
  EXPECT_EQ(output, declaration.manifest);
  EXPECT_FALSE(owner.path());
}
TEST_F(HalCourseTransferTest, CourseContextRequiresCompletedVerifiedRemovedSource) {
  removeInstalledCourse();
  ASSERT_FALSE(HasFatalFailure());
  auto& hal = inventory_hal_test::state;
  HalRemovedCourseBaseline owner(generation, scratch, [](void*) { return true; }, nullptr);
  ContentManifest output;
  auto source = CourseContextSource::None;
  const auto before = hal.files;
  ASSERT_EQ(owner.inspectCurrentCourse(output, source), CourseContextResult::Ok);
  EXPECT_EQ(output, declaration.manifest);
  EXPECT_EQ(source, CourseContextSource::Removed);
  EXPECT_FALSE(owner.path());
  EXPECT_EQ(hal.files, before);
  auto wrong = generation;
  wrong[0] ^= 2;
  HalRemovedCourseBaseline foreign(wrong, scratch, [](void*) { return true; }, nullptr);
  EXPECT_EQ(foreign.inspectCurrentCourse(output, source), CourseContextResult::Corrupt);
  EXPECT_EQ(output, declaration.manifest);
  EXPECT_EQ(source, CourseContextSource::Removed);
  EXPECT_EQ(hal.files, before);
  const auto cache = std::find_if(hal.files.begin(), hal.files.end(), [](const auto& entry) {
    return entry.first.starts_with(COURSE_REMOVAL_CACHE_PREFIX);
  });
  ASSERT_NE(cache, hal.files.end());
  cache->second[0] ^= 1;
  const auto corrupt = hal.files;
  EXPECT_EQ(owner.inspectCurrentCourse(output, source), CourseContextResult::Corrupt);
  EXPECT_EQ(output, declaration.manifest);
  EXPECT_EQ(source, CourseContextSource::Removed);
  EXPECT_FALSE(owner.path());
  EXPECT_EQ(hal.files, corrupt);
}
TEST_F(HalCourseTransferTest, NativeCourseOwnerUsesRealPreparationAndCompletedRetrySkipsLiveInventory) {
  auto& hal = inventory_hal_test::state;
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  hal.files["/tinta/items.bin"] = {17};
  std::array<uint8_t, COURSE_BINDING_SIZE> binding{};
  ASSERT_EQ(encodeCourseBinding(declaration.manifest, binding), binding.size());
  hal.files[COURSE_BINDING_PATH] = {binding.begin(), binding.end()};
  ASSERT_TRUE(storage.prepare());
  std::vector<uint8_t> paths(INVENTORY_INDEX_HEADER_SIZE + INVENTORY_PATH_MAX_RECORD);
  const auto pathSize = encodeInventoryPath(declaration.manifest, ACTIVE_COURSE_PATH,
                                            std::span(paths).subspan(INVENTORY_INDEX_HEADER_SIZE));
  ASSERT_GT(pathSize, 0u);
  paths.resize(INVENTORY_INDEX_HEADER_SIZE + pathSize);
  InventoryIndexHeader header{generation, 9, 1,
                              inventoryIndexCrc(std::span(paths).subspan(INVENTORY_INDEX_HEADER_SIZE))};
  ASSERT_EQ(encodeInventoryPathsHeader(header, paths), INVENTORY_INDEX_HEADER_SIZE);
  hal.files[InventoryPublication::PATHS] = paths;
  std::vector<uint8_t> index(INVENTORY_INDEX_HEADER_SIZE + INVENTORY_PATH_MAX_RECORD);
  const auto entrySize =
      encodeInventoryIndexEntry(declaration.manifest, std::span(index).subspan(INVENTORY_INDEX_HEADER_SIZE));
  ASSERT_GT(entrySize, 0u);
  index.resize(INVENTORY_INDEX_HEADER_SIZE + entrySize);
  header.entriesCrc = inventoryIndexCrc(std::span(index).subspan(INVENTORY_INDEX_HEADER_SIZE));
  ASSERT_EQ(encodeInventoryIndexHeader(header, index), INVENTORY_INDEX_HEADER_SIZE);
  hal.files[InventoryPublication::INDEX] = index;
  struct Context {
    HalTransferStorage* storage;
    HalCourseRemovalNativeOwner* owner = nullptr;
    bool allowed = true, inventoryReady = true;
    unsigned preparations = 0, refreshes = 0;
    static bool permitted(void* context) { return static_cast<Context*>(context)->allowed; }
    static bool inventory(void* context, uint64_t& revision) {
      revision = 9;
      return static_cast<Context*>(context)->inventoryReady;
    }
    static bool prepare(void* context, const ContentManifest& manifest, std::span<uint8_t> io) {
      auto& self = *static_cast<Context*>(context);
      ++self.preparations;
      return prepareBoundCourseRemovalState(*self.storage, manifest, io, permitted, context);
    }
    static bool refresh(void* context) {
      auto& self = *static_cast<Context*>(context);
      ++self.refreshes;
      return self.owner->closeReaders();
    }
  } context{&storage};
  ContentRemovalRequest request;
  request.manifest = declaration.manifest;
  request.generation = generation;
  request.owner = declaration.state.owner;
  request.transaction.fill(81);
  std::array<uint8_t, CONTENT_REMOVAL_REQUEST_SIZE> command{};
  std::array<uint8_t, CONTENT_REMOVAL_REPLY_SIZE> reply{};
  ASSERT_EQ(encodeContentRemovalRequest(request, command), command.size());
  HalCourseRemovalNativeOwner owner(generation, Context::permitted, Context::refresh, &context, Context::inventory,
                                    Context::prepare);
  context.owner = &owner;
  ASSERT_TRUE(owner.prepare());
  const auto before = hal.files;
  EXPECT_EQ(owner.handle(false, request.owner, command, reply), reply.size());
  EXPECT_EQ(reply[0], static_cast<uint8_t>(ContentRemovalResult::Unauthorized));
  EXPECT_EQ(hal.files, before);
  EXPECT_EQ(context.preparations, 0u);
  hal.files.at(InventoryPublication::PATHS)[0] ^= 1;
  const auto corrupt = hal.files;
  EXPECT_EQ(owner.handle(true, request.owner, command, reply), reply.size());
  EXPECT_NE(reply[0], static_cast<uint8_t>(ContentRemovalResult::Ok));
  EXPECT_EQ(hal.files, corrupt);
  EXPECT_EQ(context.preparations, 0u);
  hal.files = before;
  ASSERT_EQ(owner.handle(true, request.owner, command, reply), reply.size());
  ASSERT_EQ(reply[0], static_cast<uint8_t>(ContentRemovalResult::Ok));
  EXPECT_EQ(context.preparations, 1u);
  EXPECT_EQ(context.refreshes, 1u);
  EXPECT_FALSE(hal.files.contains(ACTIVE_COURSE_PATH));
  EXPECT_FALSE(hal.files.contains("/tinta/items.bin"));
  std::array<char, COURSE_STATE_PATH_SIZE> scoped{};
  ASSERT_TRUE(courseStatePath(request.manifest.logicalIdentity, "items.bin", scoped));
  EXPECT_EQ(hal.files.at(scoped.data()), std::vector<uint8_t>({17}));
  context.inventoryReady = false;
  hal.files.at(InventoryPublication::PATHS)[0] ^= 1;
  const auto completed = hal.files;
  EXPECT_EQ(owner.handle(true, request.owner, command, reply), reply.size());
  EXPECT_EQ(reply[0], static_cast<uint8_t>(ContentRemovalResult::Ok));
  EXPECT_EQ(context.preparations, 1u);
  EXPECT_EQ(hal.files, completed);
}
TEST_F(HalCourseTransferTest, NativeRemovalPreparationValidatesPackBeforeIsolatingLegacyState) {
  auto& hal = inventory_hal_test::state;
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  hal.files["/tinta/items.bin"] = {17};
  std::array<uint8_t, COURSE_BINDING_SIZE> binding{};
  ASSERT_EQ(encodeCourseBinding(declaration.manifest, binding), binding.size());
  hal.files[COURSE_BINDING_PATH] = {binding.begin(), binding.end()};
  auto permitted = [](void* context) { return *static_cast<bool*>(context); };
  bool allowed = false;
  const auto before = hal.files;
  EXPECT_FALSE(prepareBoundCourseRemovalState(storage, declaration.manifest, scratch, permitted, &allowed));
  EXPECT_EQ(hal.files, before);
  allowed = true;
  hal.files.at(ACTIVE_COURSE_PATH)[0] ^= 1;
  const auto corrupt = hal.files;
  EXPECT_FALSE(prepareBoundCourseRemovalState(storage, declaration.manifest, scratch, permitted, &allowed));
  EXPECT_EQ(hal.files, corrupt);
  hal.files = before;
  ASSERT_TRUE(prepareBoundCourseRemovalState(storage, declaration.manifest, scratch, permitted, &allowed));
  EXPECT_EQ(hal.files.at(ACTIVE_COURSE_PATH), bytes);
  EXPECT_EQ(hal.files.at(COURSE_BINDING_PATH), std::vector<uint8_t>(binding.begin(), binding.end()));
  EXPECT_FALSE(hal.files.contains("/tinta/items.bin"));
  std::array<char, COURSE_STATE_PATH_SIZE> scoped{};
  ASSERT_TRUE(courseStatePath(declaration.manifest.logicalIdentity, "items.bin", scoped));
  EXPECT_EQ(hal.files.at(scoped.data()), std::vector<uint8_t>({17}));
  const auto prepared = hal.files;
  EXPECT_TRUE(prepareBoundCourseRemovalState(storage, declaration.manifest, scratch, permitted, &allowed));
  EXPECT_EQ(hal.files, prepared);
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
  for (const auto& [path, content] : before) {
    ASSERT_TRUE(hal.files.contains(path));
    EXPECT_EQ(hal.files.at(path), content);
  }
  EXPECT_FALSE(storage.validateContent(ACTIVE_COURSE_PATH, "/candidate.pack", declaration.manifest, scratch));
  declaration.state.storageGeneration[0] ^= 2;
  EXPECT_FALSE(
      storage.validateContent(ACTIVE_COURSE_PATH, "/candidate.pack", declaration.manifest, declaration.state, scratch));
  for (const auto& [path, content] : before) {
    ASSERT_TRUE(hal.files.contains(path));
    EXPECT_EQ(hal.files.at(path), content);
  }
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
TEST_F(HalCourseTransferTest, ReinstallRetiresOnlyOldProofAndPreservesHistory) {
  removeInstalledCourse();
  ASSERT_FALSE(HasFatalFailure());
  auto& hal = inventory_hal_test::state;
  const auto removedFiles = hal.files;
  bytes[12] ^= 1;
  sealPack();
  Transfer transfer(storage, scratch);
  receive(transfer);
  EXPECT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::Ok);
  EXPECT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::Ok);
  EXPECT_FALSE(hal.files.contains(COURSE_REMOVAL_PROOF_PATH));
  EXPECT_EQ(hal.files.at(ACTIVE_COURSE_PATH), bytes);
  for (const auto& [path, value] : removedFiles) {
    if (path == COURSE_REMOVAL_PROOF_PATH || path == COURSE_BINDING_PATH) continue;
    ASSERT_TRUE(hal.files.contains(path)) << path;
    EXPECT_EQ(hal.files.at(path), value) << path;
  }
  removeInstalledCourse(72);
  ASSERT_FALSE(HasFatalFailure());
  EXPECT_TRUE(hal.files.contains(COURSE_REMOVAL_PROOF_PATH));
  EXPECT_FALSE(hal.files.contains(ACTIVE_COURSE_PATH));
}
TEST_F(HalCourseTransferTest, CourseProofRetirementRetriesAfterAppliedDeletionReportsFailure) {
  removeInstalledCourse();
  ASSERT_FALSE(HasFatalFailure());
  auto& hal = inventory_hal_test::state;
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  declaration.state.phase = TransferPhase::Committed;
  declaration.state.durableOffset = declaration.state.length;
  hal.failRemoveAfter = true;
  EXPECT_FALSE(storage.finalizeContentMetadata(ACTIVE_COURSE_PATH, declaration.manifest, declaration.state, scratch));
  ASSERT_FALSE(hal.files.contains(COURSE_REMOVAL_PROOF_PATH));
  const auto applied = hal.files;
  hal.failRemoveAfter = false;
  EXPECT_TRUE(storage.finalizeContentMetadata(ACTIVE_COURSE_PATH, declaration.manifest, declaration.state, scratch));
  EXPECT_EQ(hal.files, applied);
}
TEST_F(HalCourseTransferTest, CommittedCourseRetirementPreservesForeignAndCorruptEvidence) {
  removeInstalledCourse();
  ASSERT_FALSE(HasFatalFailure());
  auto& hal = inventory_hal_test::state;
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  declaration.state.phase = TransferPhase::Committed;
  declaration.state.durableOffset = declaration.state.length;
  auto initial = hal.files;
  hal.files.at(COURSE_REMOVAL_PROOF_PATH)[0] ^= 1;
  const auto corrupt = hal.files;
  EXPECT_FALSE(storage.finalizeContentMetadata(ACTIVE_COURSE_PATH, declaration.manifest, declaration.state, scratch));
  EXPECT_EQ(hal.files, corrupt);
  hal.files = initial;
  declaration.state.storageGeneration[0] ^= 2;
  EXPECT_FALSE(storage.finalizeContentMetadata(ACTIVE_COURSE_PATH, declaration.manifest, declaration.state, scratch));
  EXPECT_EQ(hal.files, initial);
  declaration.state.storageGeneration = generation;
  hal.failSyncPath = ACTIVE_COURSE_PATH;
  EXPECT_FALSE(storage.finalizeContentMetadata(ACTIVE_COURSE_PATH, declaration.manifest, declaration.state, scratch));
  EXPECT_EQ(hal.files, initial);
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

TEST_F(HalCourseTransferTest, NativeArchiveUsesCheckedLookupAndPreservesOriginalPack) {
  auto& hal = inventory_hal_test::state;
  hal.falseExists = true;
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  bool allowed = true;
  auto permitted = [](void* ctx) { return *static_cast<bool*>(ctx); };
  HalCoursePackArchive archive(scratch, permitted, &allowed);
  ASSERT_EQ(archive.publish(declaration.manifest, ACTIVE_COURSE_PATH), CourseArchiveResult::Ok);
  ASSERT_NE(archive.path(), nullptr);
  const std::string cached = archive.path();
  const std::string reference = archive.referencePath();
  const auto before = hal.files;
  EXPECT_EQ(hal.files.at(cached), bytes);
  EXPECT_EQ(*archive.manifest(), declaration.manifest);
  EXPECT_EQ(archive.publish(declaration.manifest, ACTIVE_COURSE_PATH), CourseArchiveResult::Ok);
  EXPECT_EQ(hal.files, before);
  EXPECT_EQ(hal.files.at(ACTIVE_COURSE_PATH), bytes);
  allowed = false;
  EXPECT_EQ(archive.path(), nullptr);
  allowed = true;
  EXPECT_EQ(archive.path(), nullptr);
  ASSERT_TRUE(archive.closeReaders());
  HalCoursePackArchive reopened(scratch, permitted, &allowed);
  EXPECT_EQ(reopened.open(declaration.manifest.logicalIdentity, declaration.manifest.contentHash),
            CourseArchiveResult::Ok);
  EXPECT_EQ(hal.files, before);
  hal.files.at(reference)[0] ^= 1;
  const auto corrupt = hal.files;
  EXPECT_EQ(reopened.open(declaration.manifest.logicalIdentity, declaration.manifest.contentHash),
            CourseArchiveResult::Corrupt);
  EXPECT_EQ(reopened.path(), nullptr);
  EXPECT_EQ(hal.files, corrupt);
}

TEST_F(HalCourseTransferTest, NativeArchiveResumesOnlyVerifiedOwnedPrefixWithoutTruncation) {
  auto& hal = inventory_hal_test::state;
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  HalCoursePackArchive archive(scratch, [](void*) { return true; }, nullptr);
  ASSERT_EQ(archive.publish(declaration.manifest, ACTIVE_COURSE_PATH), CourseArchiveResult::Ok);
  const std::string cached = archive.path();
  const std::string reference = archive.referencePath();
  const auto complete = hal.files;
  ASSERT_TRUE(archive.closeReaders());
  hal.files.erase(cached);
  hal.files.erase(reference);
  const auto count = std::min<size_t>(128, bytes.size());
  hal.files[cached + ".tmp"] = {bytes.begin(), bytes.begin() + count};
  hal.files.at(cached + ".tmp")[0] ^= 1;
  const auto corrupt = hal.files;
  EXPECT_EQ(archive.publish(declaration.manifest, ACTIVE_COURSE_PATH), CourseArchiveResult::Conflict);
  EXPECT_EQ(hal.files, corrupt);
  EXPECT_EQ(archive.path(), nullptr);
  hal.files.at(cached + ".tmp")[0] ^= 1;
  EXPECT_EQ(archive.publish(declaration.manifest, ACTIVE_COURSE_PATH), CourseArchiveResult::Ok);
  EXPECT_EQ(hal.files, complete);
  EXPECT_EQ(hal.files.at(cached), bytes);
  EXPECT_EQ(hal.files.at(ACTIVE_COURSE_PATH), bytes);
}

TEST_F(HalCourseTransferTest, NativeArchiveRefusesSyncCloseAndDirectoryLookupFailuresWithoutLoans) {
  auto& hal = inventory_hal_test::state;
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  HalCoursePackArchive archive(scratch, [](void*) { return true; }, nullptr);
  ASSERT_EQ(archive.publish(declaration.manifest, ACTIVE_COURSE_PATH), CourseArchiveResult::Ok);
  const std::string cached = archive.path();
  const auto before = hal.files;
  ASSERT_TRUE(archive.closeReaders());
  for (unsigned fault = 0; fault < 3; ++fault) {
    SCOPED_TRACE(fault);
    if (fault == 0) hal.failSyncPath = cached;
    if (fault == 1) hal.failClosePath = cached;
    if (fault == 2) hal.directoryErrorPath = "/.crosspoint/companion";
    EXPECT_NE(archive.open(declaration.manifest.logicalIdentity, declaration.manifest.contentHash),
              CourseArchiveResult::Ok);
    EXPECT_EQ(archive.path(), nullptr);
    EXPECT_EQ(hal.files, before);
    hal.failSyncPath.clear();
    hal.failClosePath.clear();
    hal.directoryErrorPath.clear();
    EXPECT_TRUE(archive.closeReaders());
    EXPECT_EQ(archive.open(declaration.manifest.logicalIdentity, declaration.manifest.contentHash),
              CourseArchiveResult::Ok);
  }
}

TEST_F(HalCourseTransferTest, RemovedCourseSwitchAuthorizesExactBaselineAndPreservesStateOnAbort) {
  auto& hal = inventory_hal_test::state;
  hal.files["/tinta/items.bin"] = {17};
  const auto previous = declaration.manifest;
  removeInstalledCourse();
  const auto retained = hal.files;
  ASSERT_TRUE(storage.verifyCourseSwitchSource(previous, generation, scratch));
  auto foreign = generation;
  foreign[0] ^= 2;
  EXPECT_FALSE(storage.verifyCourseSwitchSource(previous, foreign, scratch));
  EXPECT_EQ(hal.files, retained);
  declaration.manifest.logicalIdentity[0] = 8;
  bytes[12] ^= 1;
  sealPack();
  Transfer transfer(storage, scratch);
  receive(transfer, false);
  CourseSwitchRequest consent;
  consent.generation = generation;
  consent.transaction = declaration.state.transaction;
  consent.previousCourse = previous.logicalIdentity;
  consent.previousHash = previous.contentHash;
  consent.nextCourse = declaration.manifest.logicalIdentity;
  consent.nextHash = declaration.manifest.contentHash;
  std::array<uint8_t, COURSE_SWITCH_REQUEST_SIZE> body{};
  std::array<uint8_t, COURSE_SWITCH_REPLY_SIZE> reply{};
  auto wrong = consent;
  wrong.previousHash[0] ^= 1;
  ASSERT_TRUE(encodeCourseSwitchRequest(wrong, body));
  const auto before = hal.files;
  ASSERT_EQ(handleCourseSwitch(storage, transfer, generation, declaration.state.owner, body, reply, scratch),
            reply.size());
  EXPECT_EQ(reply[0], static_cast<uint8_t>(TransferResult::Invalid));
  EXPECT_EQ(hal.files, before);
  ASSERT_TRUE(encodeCourseSwitchRequest(consent, body));
  ASSERT_EQ(handleCourseSwitch(storage, transfer, generation, declaration.state.owner, body, reply, scratch),
            reply.size());
  ASSERT_EQ(reply[0], static_cast<uint8_t>(TransferResult::Ok));
  ASSERT_EQ(transfer.abort(declaration.state.transaction, declaration.state.owner), TransferResult::Ok);
  EXPECT_FALSE(hal.files.contains(COURSE_SWITCH_INTENT_PATH));
  EXPECT_FALSE(hal.files.contains(ACTIVE_COURSE_PATH));
  for (const auto& [path, bytes] : retained) EXPECT_EQ(hal.files.at(path), bytes);
  EXPECT_TRUE(storage.verifyCourseSwitchSource(previous, generation, scratch));
  Transfer restarted(storage, scratch);
  EXPECT_EQ(restarted.recover(generation), TransferResult::Ok);
}

TEST_F(HalCourseTransferTest, RemovedCourseSwitchRecoversPublicationAndProofRetirementInterruptions) {
  auto& hal = inventory_hal_test::state;
  hal.files["/tinta/items.bin"] = {17};
  hal.files["/tinta/starred.bin"] = {23};
  const auto previous = declaration.manifest;
  removeInstalledCourse();
  const auto baseline = hal;
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
  auto exercise = [&](unsigned rename, bool after, const char* sync, bool deletion) {
    hal = baseline;
    HalTransferStorage initial;
    Transfer transfer(initial, scratch);
    receive(transfer, false);
    std::array<uint8_t, COURSE_SWITCH_REQUEST_SIZE> body{};
    std::array<uint8_t, COURSE_SWITCH_REPLY_SIZE> reply{};
    EXPECT_TRUE(encodeCourseSwitchRequest(consent, body));
    EXPECT_EQ(handleCourseSwitch(initial, transfer, generation, declaration.state.owner, body, reply, scratch),
              reply.size());
    EXPECT_EQ(reply[0], static_cast<uint8_t>(TransferResult::Ok));
    const auto before = hal.renames;
    if (after)
      hal.failRenameAfter = before + rename;
    else if (rename)
      hal.failRename = before + rename;
    if (sync) hal.failSyncPath = sync;
    hal.failRemoveAfter = deletion;
    const auto committed = transfer.commit(declaration.state.transaction, declaration.state.owner);
    if (rename || sync || deletion)
      EXPECT_NE(committed, TransferResult::Ok);
    else
      EXPECT_EQ(committed, TransferResult::Ok);
    const auto count = hal.renames - before;
    hal.failRename = hal.failRenameAfter = 0;
    hal.failSyncPath.clear();
    hal.failRemoveAfter = false;
    HalTransferStorage reopened;
    Transfer recovered(reopened, scratch);
    EXPECT_EQ(recovered.recover(generation), TransferResult::Ok);
    EXPECT_EQ(recovered.commit(declaration.state.transaction, declaration.state.owner), TransferResult::Ok);
    EXPECT_EQ(hal.files.at(ACTIVE_COURSE_PATH), bytes);
    EXPECT_FALSE(hal.files.contains(COURSE_REMOVAL_PROOF_PATH));
    EXPECT_FALSE(hal.files.contains(COURSE_SWITCH_INTENT_PATH));
    ContentManifest installed;
    EXPECT_TRUE(decodeCourseBinding(hal.files.at(COURSE_BINDING_PATH), installed));
    EXPECT_EQ(installed, declaration.manifest);
    for (const auto& [path, data] : baseline.files) {
      if (path != COURSE_REMOVAL_PROOF_PATH && path != COURSE_BINDING_PATH) {
        EXPECT_EQ(hal.files.at(path), data);
      }
    }
    EXPECT_TRUE(reopened.verifyCourseSwitchSource(declaration.manifest, generation, scratch));
    return count;
  };
  const auto renames = exercise(0, false, nullptr, false);
  ASSERT_GT(renames, 0);
  for (unsigned rename = 1; rename <= renames; ++rename)
    for (bool after : {false, true}) {
      SCOPED_TRACE(rename);
      SCOPED_TRACE(after);
      exercise(rename, after, nullptr, false);
    }
  for (const auto* path : {COURSE_BINDING_STAGE, TRANSFER_JOURNALS[0], TRANSFER_JOURNALS[1]}) {
    SCOPED_TRACE(path);
    exercise(0, false, path, false);
  }
  exercise(0, false, nullptr, true);
}

TEST_F(HalCourseTransferTest, ReturnToRemovedCourseUsesRetainedReceiptsAfterProofRetirement) {
  addIdentityHistory();
  auto& hal = inventory_hal_test::state;
  hal.files["/tinta/items.bin"] = {17};
  const auto originalDeclaration = declaration;
  const auto originalBytes = bytes;
  removeInstalledCourse();
  auto authorize = [&](HalTransferStorage& target, Transfer& transfer, const ContentManifest& previous) {
    CourseSwitchRequest consent;
    consent.generation = generation;
    consent.transaction = declaration.state.transaction;
    consent.previousCourse = previous.logicalIdentity;
    consent.previousHash = previous.contentHash;
    consent.nextCourse = declaration.manifest.logicalIdentity;
    consent.nextHash = declaration.manifest.contentHash;
    std::array<uint8_t, COURSE_SWITCH_REQUEST_SIZE> body{};
    std::array<uint8_t, COURSE_SWITCH_REPLY_SIZE> reply{};
    ASSERT_TRUE(encodeCourseSwitchRequest(consent, body));
    ASSERT_EQ(handleCourseSwitch(target, transfer, generation, declaration.state.owner, body, reply, scratch),
              reply.size());
    ASSERT_EQ(reply[0], static_cast<uint8_t>(TransferResult::Ok));
  };
  declaration.manifest.logicalIdentity[0] = 8;
  bytes[12] ^= 1;
  sealPack();
  {
    Transfer transfer(storage, scratch);
    receive(transfer, false);
    ASSERT_FALSE(HasFatalFailure());
    authorize(storage, transfer, originalDeclaration.manifest);
    ASSERT_FALSE(HasFatalFailure());
    ASSERT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::Ok);
  }
  const auto current = declaration.manifest;
  const auto currentBytes = bytes;
  ASSERT_FALSE(hal.files.contains(COURSE_REMOVAL_PROOF_PATH));
  char oldState[COURSE_STATE_PATH_SIZE], currentState[COURSE_STATE_PATH_SIZE];
  ASSERT_TRUE(courseStatePath(originalDeclaration.manifest.logicalIdentity, "items.bin", oldState));
  ASSERT_TRUE(courseStatePath(current.logicalIdentity, "items.bin", currentState));
  hal.files[currentState] = {31};
  HalCoursePackArchive archive(scratch, [](void*) { return true; }, nullptr);
  ASSERT_EQ(archive.open(originalDeclaration.manifest.logicalIdentity, originalDeclaration.manifest.contentHash),
            CourseArchiveResult::Ok);
  const std::string cache = archive.path();
  const std::string reference = archive.referencePath();
  ASSERT_TRUE(archive.closeReaders());
  // Older installations retained removal evidence without immutable pack archives.
  hal.files.erase(cache);
  hal.files.erase(cache + ".owner");
  hal.files.erase(reference);
  const auto baseline = hal;
  for (unsigned fault = 0; fault < 4; ++fault) {
    SCOPED_TRACE(fault);
    hal = baseline;
    declaration = originalDeclaration;
    declaration.state.transaction[0] = 5;
    bytes = originalBytes;
    if (fault == 1) bytes.back() ^= 1;
    if (fault == 2) bytes[offsetof(tinta::core::pack::Header, locale)] = 'f';
    sealPack();
    if (fault == 3) {
      bool corrupted = false;
      for (auto& [path, data] : hal.files) {
        if (path.starts_with("/.crosspoint/companion/course-removed-")) {
          ASSERT_FALSE(data.empty());
          data.back() ^= 1;
          corrupted = true;
        }
      }
      ASSERT_TRUE(corrupted);
    }
    HalTransferStorage reopened;
    Transfer transfer(reopened, scratch);
    receive(transfer, false);
    ASSERT_FALSE(HasFatalFailure());
    authorize(reopened, transfer, current);
    ASSERT_FALSE(HasFatalFailure());
    const auto result = transfer.commit(declaration.state.transaction, declaration.state.owner);
    ContentManifest installed;
    ASSERT_TRUE(decodeCourseBinding(hal.files.at(COURSE_BINDING_PATH), installed));
    if (fault) {
      EXPECT_NE(result, TransferResult::Ok);
      EXPECT_EQ(installed, current);
      EXPECT_EQ(hal.files.at(ACTIVE_COURSE_PATH), currentBytes);
    } else {
      EXPECT_EQ(result, TransferResult::Ok);
      EXPECT_EQ(installed, declaration.manifest);
      EXPECT_EQ(hal.files.at(ACTIVE_COURSE_PATH), originalBytes);
      EXPECT_TRUE(hal.files.contains(cache));
      EXPECT_TRUE(hal.files.contains(reference));
    }
    EXPECT_EQ(hal.files.at(oldState), std::vector<uint8_t>({17}));
    EXPECT_EQ(hal.files.at(currentState), std::vector<uint8_t>({31}));
    EXPECT_FALSE(hal.files.contains(COURSE_REMOVAL_PROOF_PATH));
  }
}

TEST_F(HalCourseTransferTest, SwitchedProofRetirementRequiresExactConsentAndRetriesAppliedDeletion) {
  const auto previous = declaration.manifest;
  removeInstalledCourse();
  ASSERT_FALSE(HasFatalFailure());
  auto& hal = inventory_hal_test::state;
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
  CourseSwitchIntent intent(storage, scratch);
  ASSERT_EQ(intent.persist(consent), CourseSwitchIntentResult::Ok);
  char directory[COURSE_STATE_DIRECTORY_SIZE];
  ASSERT_TRUE(courseStateDirectory(declaration.manifest.logicalIdentity, directory));
  ASSERT_TRUE(Storage.ensureDirectoryExists(directory));
  std::array<uint8_t, COURSE_BINDING_SIZE> binding{};
  ASSERT_EQ(encodeCourseBinding(declaration.manifest, binding), binding.size());
  hal.files[COURSE_BINDING_PATH] = {binding.begin(), binding.end()};
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  declaration.state.phase = TransferPhase::Committed;
  declaration.state.durableOffset = declaration.state.length;
  const auto before = hal;
  HalRemovedCourseBaseline retirement(generation, scratch, [](void*) { return true; }, nullptr);
  EXPECT_FALSE(retirement.retireInstalled(declaration.manifest, declaration.state));
  EXPECT_EQ(hal.files, before.files);
  auto wrong = consent;
  wrong.previousHash[0] ^= 1;
  EXPECT_FALSE(retirement.retireInstalled(declaration.manifest, declaration.state, &wrong));
  EXPECT_EQ(hal.files, before.files);
  for (const auto& [path, data] : before.files) {
    if (path != COURSE_REMOVAL_PROOF_PATH && !path.starts_with("/.crosspoint/companion/course-removed-")) continue;
    hal.files.at(path).back() ^= 1;
    const auto corrupt = hal.files;
    EXPECT_FALSE(storage.finalizeContentMetadata(ACTIVE_COURSE_PATH, declaration.manifest, declaration.state, scratch));
    EXPECT_EQ(hal.files, corrupt);
    hal.files = before.files;
  }
  auto foreign = declaration.state;
  foreign.storageGeneration[0] ^= 2;
  EXPECT_FALSE(storage.finalizeContentMetadata(ACTIVE_COURSE_PATH, declaration.manifest, foreign, scratch));
  EXPECT_EQ(hal.files, before.files);
  hal.failRemove = true;
  EXPECT_FALSE(storage.finalizeContentMetadata(ACTIVE_COURSE_PATH, declaration.manifest, declaration.state, scratch));
  EXPECT_EQ(hal.files, before.files);
  hal.failRemove = false;
  hal.failRemoveAfter = true;
  EXPECT_FALSE(storage.finalizeContentMetadata(ACTIVE_COURSE_PATH, declaration.manifest, declaration.state, scratch));
  EXPECT_FALSE(hal.files.contains(COURSE_REMOVAL_PROOF_PATH));
  EXPECT_TRUE(hal.files.contains(COURSE_SWITCH_INTENT_PATH));
  EXPECT_FALSE(storage.finalizeContentMetadata(ACTIVE_COURSE_PATH, declaration.manifest, declaration.state, scratch));
  EXPECT_FALSE(hal.files.contains(COURSE_SWITCH_INTENT_PATH));
  hal.failRemoveAfter = false;
  const auto applied = hal.files;
  EXPECT_TRUE(storage.finalizeContentMetadata(ACTIVE_COURSE_PATH, declaration.manifest, declaration.state, scratch));
  EXPECT_EQ(hal.files, applied);
  for (const auto& [path, data] : before.files) {
    if (path != COURSE_REMOVAL_PROOF_PATH && path != COURSE_SWITCH_INTENT_PATH) {
      EXPECT_EQ(hal.files.at(path), data);
    }
  }
}

TEST_F(HalCourseTransferTest, ConsentedCourseSwitchInstallsAndRecoversWithSeparateState) {
  auto& hal = inventory_hal_test::state;
  hal.directories["/"] = {};
  hal.directories["/tinta"] = {};
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
  hal.directories["/"] = {};
  hal.directories["/tinta"] = {};
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
    hal.enumerateFileMap = true;
    HalTransferStorage initialStorage;
    hal.directories["/"] = {};
    hal.directories["/tinta"] = {};
    hal.files[ACTIVE_COURSE_PATH] = oldPack;
    hal.files[COURSE_BINDING_PATH] = {oldBinding.begin(), oldBinding.end()};
    hal.files["/tinta/items.bin"] = {17};
    hal.files["/tinta/starred.bin"] = {23};
    char nextPath[COURSE_STATE_PATH_SIZE];
    EXPECT_TRUE(courseStatePath(declaration.manifest.logicalIdentity, "items.bin", nextPath));
    hal.files["/historical-target.pack"] = bytes;
    HalCoursePackArchive targetArchive(scratch, [](void*) { return true; }, nullptr);
    EXPECT_EQ(targetArchive.publish(declaration.manifest, "/historical-target.pack"), CourseArchiveResult::Ok);
    EXPECT_TRUE(targetArchive.closeReaders());
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
    inventory_hal_test::state.enumerateFileMap = true;
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

TEST_F(HalCourseTransferTest, CourseHistoryVisitsVerifiedVersionsWithoutChangingState) {
  auto& hal = inventory_hal_test::state;
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  HalCoursePackArchive archive(scratch, [](void*) { return true; }, nullptr);
  ASSERT_EQ(archive.publish(declaration.manifest, ACTIVE_COURSE_PATH), CourseArchiveResult::Ok);
  ASSERT_TRUE(archive.closeReaders());
  const auto before = hal.files;
  HalCoursePackHistory history(scratch, [](void*) { return true; }, nullptr);
  struct Visits {
    unsigned count = 0;
    ContentManifest manifest{};
  } visits;
  auto visitor = [](void* ctx, const ContentManifest& manifest, const char* path) {
    auto& result = *static_cast<Visits*>(ctx);
    ++result.count;
    result.manifest = manifest;
    return path && inventory_hal_test::state.files.contains(path);
  };
  EXPECT_EQ(history.visit(declaration.manifest.logicalIdentity, visitor, &visits), CourseHistoryResult::Ok);
  EXPECT_EQ(visits.count, 1u);
  EXPECT_EQ(visits.manifest, declaration.manifest);
  EXPECT_EQ(hal.files, before);
  EXPECT_EQ(history.visit(
                declaration.manifest.logicalIdentity, [](void*, const ContentManifest&, const char*) { return false; },
                nullptr),
            CourseHistoryResult::Incompatible);
  EXPECT_EQ(hal.files, before);
}

TEST_F(HalCourseTransferTest, CourseHistoryDistinguishesFreshScopeFromMissingBaseline) {
  auto& hal = inventory_hal_test::state;
  char scope[COURSE_STATE_DIRECTORY_SIZE];
  ASSERT_TRUE(courseStateDirectory(declaration.manifest.logicalIdentity, scope));
  hal.directories["/tinta/courses"] = {};
  hal.directories[scope] = {};
  HalCoursePackHistory history(scratch, [](void*) { return true; }, nullptr);
  unsigned visits = 0;
  auto visitor = [](void* ctx, const ContentManifest&, const char*) {
    ++*static_cast<unsigned*>(ctx);
    return true;
  };
  EXPECT_EQ(history.visit(declaration.manifest.logicalIdentity, visitor, &visits), CourseHistoryResult::Ok);
  hal.files[std::string(scope) + "/items.bin"] = {17};
  const auto before = hal.files;
  EXPECT_EQ(history.visit(declaration.manifest.logicalIdentity, visitor, &visits),
            CourseHistoryResult::MissingBaseline);
  EXPECT_EQ(visits, 0u);
  EXPECT_EQ(hal.files, before);
}

TEST_F(HalCourseTransferTest, CourseHistoryRefusesUnfinishedMalformedAndCorruptReferences) {
  auto& hal = inventory_hal_test::state;
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  HalCoursePackArchive archive(scratch, [](void*) { return true; }, nullptr);
  ASSERT_EQ(archive.publish(declaration.manifest, ACTIVE_COURSE_PATH), CourseArchiveResult::Ok);
  const std::string reference = archive.referencePath();
  ASSERT_TRUE(archive.closeReaders());
  const auto complete = hal.files;
  HalCoursePackHistory history(scratch, [](void*) { return true; }, nullptr);
  unsigned visits = 0;
  auto visitor = [](void* ctx, const ContentManifest&, const char*) {
    ++*static_cast<unsigned*>(ctx);
    return true;
  };
  for (unsigned fault = 0; fault < 3; ++fault) {
    SCOPED_TRACE(fault);
    hal.files = complete;
    if (fault == 0) hal.files[reference.substr(0, reference.size() - 4) + ".tmp"] = {1};
    if (fault == 1) hal.files[reference.substr(0, reference.rfind('/') + 1) + "pack-bad.ref"] = {1};
    if (fault == 2) hal.files.at(reference)[0] ^= 1;
    const auto before = hal.files;
    EXPECT_EQ(history.visit(declaration.manifest.logicalIdentity, visitor, &visits),
              fault == 0 ? CourseHistoryResult::Busy : CourseHistoryResult::Corrupt);
    EXPECT_EQ(visits, 0u);
    EXPECT_EQ(hal.files, before);
  }
}

TEST_F(HalCourseTransferTest, CourseHistoryRefusesIncompleteEnumerationAndPermissionLoss) {
  auto& hal = inventory_hal_test::state;
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  HalCoursePackArchive archive(scratch, [](void*) { return true; }, nullptr);
  ASSERT_EQ(archive.publish(declaration.manifest, ACTIVE_COURSE_PATH), CourseArchiveResult::Ok);
  ASSERT_TRUE(archive.closeReaders());
  const auto before = hal.files;
  bool allowed = true;
  HalCoursePackHistory history(scratch, [](void* ctx) { return *static_cast<bool*>(ctx); }, &allowed);
  auto visitor = [](void* ctx, const ContentManifest&, const char*) {
    *static_cast<bool*>(ctx) = false;
    return true;
  };
  hal.directoryErrorPath = "/tinta/courses";
  EXPECT_EQ(history.visit(declaration.manifest.logicalIdentity, visitor, &allowed), CourseHistoryResult::IoError);
  EXPECT_TRUE(allowed);
  hal.directoryErrorPath.clear();
  EXPECT_EQ(history.visit(declaration.manifest.logicalIdentity, visitor, &allowed), CourseHistoryResult::Busy);
  EXPECT_FALSE(allowed);
  EXPECT_EQ(hal.files, before);
}

TEST_F(HalCourseTransferTest, CourseHistoryRejectsDuplicateFoldedCourseDirectories) {
  auto& hal = inventory_hal_test::state;
  auto manifest = declaration.manifest;
  manifest.logicalIdentity[0] = 0xaf;
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  HalCoursePackArchive archive(scratch, [](void*) { return true; }, nullptr);
  ASSERT_EQ(archive.publish(manifest, ACTIVE_COURSE_PATH), CourseArchiveResult::Ok);
  ASSERT_TRUE(archive.closeReaders());
  char scope[COURSE_STATE_DIRECTORY_SIZE];
  ASSERT_TRUE(courseStateDirectory(manifest.logicalIdentity, scope));
  std::string duplicate = scope;
  duplicate[COURSE_STATE_ROOT.size()] = 'A';
  duplicate[COURSE_STATE_ROOT.size() + 1] = 'F';
  hal.directories[duplicate] = {};
  const auto before = hal.files;
  unsigned visits = 0;
  HalCoursePackHistory history(scratch, [](void*) { return true; }, nullptr);
  EXPECT_EQ(history.visit(
                manifest.logicalIdentity,
                [](void* ctx, const ContentManifest&, const char*) {
                  ++*static_cast<unsigned*>(ctx);
                  return true;
                },
                &visits),
            CourseHistoryResult::IoError);
  EXPECT_EQ(visits, 0u);
  EXPECT_EQ(hal.files, before);
}

TEST_F(HalCourseTransferTest, InstalledCourseMetadataRetainsArchiveBeforeCompletionAndRetriesCloseFailure) {
  auto& hal = inventory_hal_test::state;
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  declaration.state.phase = TransferPhase::Installing;
  declaration.state.durableOffset = declaration.state.length;
  ASSERT_TRUE(storage.installContentMetadata(ACTIVE_COURSE_PATH, declaration.manifest, declaration.state, scratch));
  HalCoursePackArchive archive(scratch, [](void*) { return true; }, nullptr);
  ASSERT_EQ(archive.open(declaration.manifest.logicalIdentity, declaration.manifest.contentHash),
            CourseArchiveResult::Ok);
  const std::string cached = archive.path();
  const std::string reference = archive.referencePath();
  ASSERT_TRUE(archive.closeReaders());
  const auto complete = hal.files;
  hal.failClosePath = cached;
  EXPECT_FALSE(storage.installContentMetadata(ACTIVE_COURSE_PATH, declaration.manifest, declaration.state, scratch));
  EXPECT_EQ(hal.files, complete);
  hal.failClosePath.clear();
  EXPECT_TRUE(storage.installContentMetadata(ACTIVE_COURSE_PATH, declaration.manifest, declaration.state, scratch));
  EXPECT_EQ(hal.files, complete);
  EXPECT_EQ(hal.files.at(cached), bytes);
  EXPECT_TRUE(hal.files.contains(reference));
  hal.files.erase(ACTIVE_COURSE_PATH);
  const auto retained = hal.files;
  struct HistoryCheck {
    HalTransferStorage* storage;
    std::span<uint8_t> scratch;
    const ContentManifest* expected;
    unsigned count = 0;
  } check{&storage, scratch, &declaration.manifest};
  HalCoursePackHistory history(scratch, [](void*) { return true; }, nullptr);
  EXPECT_EQ(history.visit(
                declaration.manifest.logicalIdentity,
                [](void* context, const ContentManifest& manifest, const char* path) {
                  auto& check = *static_cast<HistoryCheck*>(context);
                  ++check.count;
                  return manifest == *check.expected &&
                         check.storage->verify(path, manifest.length, manifest.contentHash, check.scratch);
                },
                &check),
            CourseHistoryResult::Ok);
  EXPECT_EQ(check.count, 1u);
  EXPECT_EQ(hal.files, retained);
}

TEST_F(HalCourseTransferTest, CourseHistoryVisitsEveryFullyValidatedPackVersionAfterActiveRemoval) {
  addIdentityHistory();
  auto& hal = inventory_hal_test::state;
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  const auto previous = declaration.manifest;
  HalCoursePackArchive archive(scratch, [](void*) { return true; }, nullptr);
  ASSERT_EQ(archive.publish(previous, ACTIVE_COURSE_PATH), CourseArchiveResult::Ok);
  ASSERT_TRUE(archive.closeReaders());
  bytes[12] ^= 1;  // Build timestamp changes without altering item meaning.
  sealPack();
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  const auto next = declaration.manifest;
  ASSERT_NE(previous.contentHash, next.contentHash);
  ASSERT_EQ(archive.publish(next, ACTIVE_COURSE_PATH), CourseArchiveResult::Ok);
  ASSERT_TRUE(archive.closeReaders());
  hal.files.erase(ACTIVE_COURSE_PATH);
  const auto retained = hal.files;
  auto pack = makeUniqueNoThrow<tinta::core::pack::Pack>();
  ASSERT_TRUE(pack);
  struct Check {
    tinta::core::pack::Pack* pack;
    std::span<uint8_t> scratch;
    const ContentManifest* previous;
    const ContentManifest* next;
    unsigned previousCount = 0, nextCount = 0;
  } check{pack.get(), scratch, &previous, &next};
  HalCoursePackHistory history(scratch, [](void*) { return true; }, nullptr);
  EXPECT_EQ(history.visit(
                previous.logicalIdentity,
                [](void* context, const ContentManifest& manifest, const char* path) {
                  auto& check = *static_cast<Check*>(context);
                  CourseCandidateDetails details;
                  if (!validateStagedCourse(path, *check.pack, check.scratch, details)) return false;
                  if (manifest == *check.previous)
                    ++check.previousCount;
                  else if (manifest == *check.next)
                    ++check.nextCount;
                  else
                    return false;
                  return details.major == manifest.formatVersion;
                },
                &check),
            CourseHistoryResult::Ok);
  EXPECT_EQ(check.previousCount, 1u);
  EXPECT_EQ(check.nextCount, 1u);
  EXPECT_EQ(hal.files, retained);
}

TEST_F(HalCourseTransferTest, CourseMetadataRejectsIncompleteOrForeignTransferContextWithoutWrites) {
  auto& hal = inventory_hal_test::state;
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  const auto before = hal.files;
  const auto directoryCount = hal.directories.size();
  for (unsigned fault = 0; fault < 8; ++fault) {
    SCOPED_TRACE(fault);
    auto state = declaration.state;
    state.phase = TransferPhase::Installing;
    state.durableOffset = state.length;
    switch (fault) {
      case 0:
        state.contentHash[0] ^= 1;
        break;
      case 1:
        state.owner = {};
        break;
      case 2:
        state.transaction = {};
        break;
      case 3:
        state.storageGeneration = {};
        break;
      case 4:
        --state.durableOffset;
        break;
      case 5:
        state.phase = TransferPhase::Verified;
        break;
      case 6:
        state.phase = TransferPhase::Receiving;
        break;
      case 7:
        state.phase = TransferPhase::Aborted;
        break;
    }
    EXPECT_FALSE(storage.installContentMetadata(ACTIVE_COURSE_PATH, declaration.manifest, state, scratch));
    EXPECT_EQ(hal.files, before);
    EXPECT_EQ(hal.directories.size(), directoryCount);
  }
}

TEST_F(HalCourseTransferTest, ArchivedHistoryValidatorAcceptsTimestampUpdateAndRejectsRetiredUidReinterpretation) {
  addIdentityHistory();
  auto& hal = inventory_hal_test::state;
  const auto previous = declaration.manifest;
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  HalCoursePackArchive archive(scratch, [](void*) { return true; }, nullptr);
  ASSERT_EQ(archive.publish(previous, ACTIVE_COURSE_PATH), CourseArchiveResult::Ok);
  ASSERT_TRUE(archive.closeReaders());
  bytes[12] ^= 1;
  sealPack();
  hal.files["/candidate.pack"] = bytes;
  auto parser = makeUniqueNoThrow<tinta::core::pack::Pack>();
  ASSERT_TRUE(parser);
  HalCoursePackHistory history(scratch, [](void*) { return true; }, nullptr);
  HalCoursePackHistoryValidator validator(storage, *parser, scratch);
  EXPECT_EQ(validator.validate(history, declaration.manifest, "/candidate.pack"), CourseHistoryResult::Ok);
  bytes.back() ^= 1;  // Change the retained meaning of the retired UID.
  sealPack();
  hal.files["/candidate.pack"] = bytes;
  const auto before = hal.files;
  EXPECT_EQ(validator.validate(history, declaration.manifest, "/candidate.pack"), CourseHistoryResult::Incompatible);
  EXPECT_EQ(hal.files, before);
}

TEST_F(HalCourseTransferTest, ArchivedLegacyHistoryRequiresSameRecordsAndLanguage) {
  auto& hal = inventory_hal_test::state;
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  HalCoursePackArchive archive(scratch, [](void*) { return true; }, nullptr);
  ASSERT_EQ(archive.publish(declaration.manifest, ACTIVE_COURSE_PATH), CourseArchiveResult::Ok);
  ASSERT_TRUE(archive.closeReaders());
  auto parser = makeUniqueNoThrow<tinta::core::pack::Pack>();
  ASSERT_TRUE(parser);
  HalCoursePackHistory history(scratch, [](void*) { return true; }, nullptr);
  HalCoursePackHistoryValidator validator(storage, *parser, scratch);
  bytes[12] ^= 1;
  sealPack();
  hal.files["/candidate.pack"] = bytes;
  EXPECT_EQ(validator.validate(history, declaration.manifest, "/candidate.pack"), CourseHistoryResult::Ok);
  constexpr auto localeOffset = offsetof(tinta::core::pack::Header, locale);
  bytes[localeOffset] = bytes[localeOffset] == 'e' ? 'f' : 'e';
  sealPack();
  hal.files["/candidate.pack"] = bytes;
  const auto before = hal.files;
  EXPECT_EQ(validator.validate(history, declaration.manifest, "/candidate.pack"), CourseHistoryResult::Incompatible);
  EXPECT_EQ(hal.files, before);
}

TEST_F(HalCourseTransferTest, ExplicitReturnToCourseRejectsChangedHistoricalUidBeforePackReplacement) {
  addIdentityHistory();
  auto& hal = inventory_hal_test::state;
  hal.directories["/"] = {};
  const auto previous = declaration.manifest;
  const auto oldPack = bytes;
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  std::array<uint8_t, COURSE_BINDING_SIZE> binding{};
  ASSERT_EQ(encodeCourseBinding(previous, binding), binding.size());
  hal.files[COURSE_BINDING_PATH] = {binding.begin(), binding.end()};
  hal.files["/tinta/items.bin"] = {17};
  bytes[12] ^= 1;
  sealPack();
  auto historical = declaration.manifest;
  historical.logicalIdentity[0] = 8;
  hal.files["/historical-target.pack"] = bytes;
  HalCoursePackArchive archive(scratch, [](void*) { return true; }, nullptr);
  ASSERT_EQ(archive.publish(historical, "/historical-target.pack"), CourseArchiveResult::Ok);
  ASSERT_TRUE(archive.closeReaders());
  char targetState[COURSE_STATE_PATH_SIZE];
  ASSERT_TRUE(courseStatePath(historical.logicalIdentity, "items.bin", targetState));
  hal.files[targetState] = {31};
  declaration.manifest.logicalIdentity = historical.logicalIdentity;
  bytes.back() ^= 1;  // Retired UID meaning conflicts with the earlier target pack.
  sealPack();
  hal.files["/candidate.pack"] = bytes;
  declaration.state.durableOffset = declaration.state.length;
  declaration.state.phase = TransferPhase::Receiving;
  CourseSwitchRequest request;
  request.generation = generation;
  request.transaction = declaration.state.transaction;
  request.previousCourse = previous.logicalIdentity;
  request.previousHash = previous.contentHash;
  request.nextCourse = declaration.manifest.logicalIdentity;
  request.nextHash = declaration.manifest.contentHash;
  CourseSwitchIntent intent(storage, scratch);
  ASSERT_EQ(intent.persist(request), CourseSwitchIntentResult::Ok);
  EXPECT_FALSE(
      storage.validateContent(ACTIVE_COURSE_PATH, "/candidate.pack", declaration.manifest, declaration.state, scratch));
  EXPECT_EQ(hal.files.at(ACTIVE_COURSE_PATH), oldPack);
  EXPECT_EQ(hal.files.at(COURSE_BINDING_PATH), std::vector<uint8_t>(binding.begin(), binding.end()));
  EXPECT_EQ(hal.files.at(targetState), std::vector<uint8_t>({31}));
  char oldState[COURSE_STATE_PATH_SIZE];
  ASSERT_TRUE(courseStatePath(previous.logicalIdentity, "items.bin", oldState));
  EXPECT_EQ(hal.files.at(oldState), std::vector<uint8_t>({17}));
  ASSERT_EQ(archive.open(previous.logicalIdentity, previous.contentHash), CourseArchiveResult::Ok);
  EXPECT_EQ(hal.files.at(archive.path()), oldPack);
}

TEST_F(HalCourseTransferTest, ExplicitReturnToCoursePreservesUsedScopeWithoutHistoricalBaseline) {
  addIdentityHistory();
  auto& hal = inventory_hal_test::state;
  hal.directories["/"] = {};
  hal.directories["/tinta"] = {};
  hal.directories[TRANSFER_DIRECTORY] = {};
  const auto previous = declaration.manifest;
  const auto oldPack = bytes;
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  std::array<uint8_t, COURSE_BINDING_SIZE> binding{};
  ASSERT_EQ(encodeCourseBinding(previous, binding), binding.size());
  hal.files[COURSE_BINDING_PATH] = {binding.begin(), binding.end()};
  hal.files["/tinta/items.bin"] = {17};
  bytes[12] ^= 1;
  sealPack();
  auto historical = declaration.manifest;
  historical.logicalIdentity[0] = 8;
  char targetState[COURSE_STATE_PATH_SIZE];
  ASSERT_TRUE(courseStatePath(historical.logicalIdentity, "items.bin", targetState));
  hal.files[targetState] = {31};
  declaration.manifest.logicalIdentity = historical.logicalIdentity;
  sealPack();
  hal.files["/candidate.pack"] = bytes;
  declaration.state.durableOffset = declaration.state.length;
  declaration.state.phase = TransferPhase::Receiving;
  CourseSwitchRequest request;
  request.generation = generation;
  request.transaction = declaration.state.transaction;
  request.previousCourse = previous.logicalIdentity;
  request.previousHash = previous.contentHash;
  request.nextCourse = declaration.manifest.logicalIdentity;
  request.nextHash = declaration.manifest.contentHash;
  CourseSwitchIntent intent(storage, scratch);
  ASSERT_EQ(intent.persist(request), CourseSwitchIntentResult::Ok);
  EXPECT_FALSE(
      storage.validateContent(ACTIVE_COURSE_PATH, "/candidate.pack", declaration.manifest, declaration.state, scratch));
  EXPECT_EQ(hal.files.at(ACTIVE_COURSE_PATH), oldPack);
  EXPECT_EQ(hal.files.at(COURSE_BINDING_PATH), std::vector<uint8_t>(binding.begin(), binding.end()));
  EXPECT_EQ(hal.files.at(targetState), std::vector<uint8_t>({31}));
  char oldState[COURSE_STATE_PATH_SIZE];
  ASSERT_TRUE(courseStatePath(previous.logicalIdentity, "items.bin", oldState));
  ASSERT_TRUE(hal.files.contains(oldState));
  EXPECT_EQ(hal.files.at(oldState), std::vector<uint8_t>({17}));
  HalCoursePackArchive archive(scratch, [](void*) { return true; }, nullptr);
  ASSERT_EQ(archive.open(previous.logicalIdentity, previous.contentHash), CourseArchiveResult::Ok);
  EXPECT_EQ(hal.files.at(archive.path()), oldPack);
}

TEST_F(HalCourseTransferTest, HistoricalRemovedCourseBaselineSurvivesDifferentActiveBindingAndProofRetirement) {
  auto& hal = inventory_hal_test::state;
  hal.files["/tinta/items.bin"] = {17};
  const auto previous = declaration.manifest;
  const auto original = bytes;
  removeInstalledCourse();
  ContentRemovalRecord completed;
  bool found = false;
  for (const auto& [path, record] : hal.files) {
    if (path.starts_with("/.crosspoint/companion/removal-done-") && decodeContentRemovalRecord(record, completed)) {
      found = true;
      break;
    }
  }
  ASSERT_TRUE(found);
  hal.files.erase(COURSE_REMOVAL_PROOF_PATH);
  declaration.manifest.logicalIdentity[0] = 8;
  bytes[12] ^= 1;
  sealPack();
  std::array<uint8_t, COURSE_BINDING_SIZE> binding{};
  ASSERT_EQ(encodeCourseBinding(declaration.manifest, binding), binding.size());
  hal.files[COURSE_BINDING_PATH] = {binding.begin(), binding.end()};
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  const auto before = hal.files;
  bool allowed = true;
  HalHistoricalCourseBaseline source(
      generation, previous.logicalIdentity, scratch, [](void* ctx) { return *static_cast<bool*>(ctx); }, &allowed);
  ASSERT_TRUE(source.open(completed));
  ASSERT_NE(source.path(), nullptr);
  const std::string cached = source.path();
  EXPECT_EQ(*source.manifest(), previous);
  EXPECT_EQ(hal.files.at(cached), original);
  EXPECT_EQ(hal.files, before);
  allowed = false;
  EXPECT_EQ(source.path(), nullptr);
  allowed = true;
  EXPECT_EQ(source.path(), nullptr);
  ASSERT_TRUE(source.open(completed));
  ASSERT_TRUE(source.closeReaders());
  hal.files.at(cached)[0] ^= 1;
  const auto corrupt = hal.files;
  EXPECT_FALSE(source.open(completed));
  EXPECT_EQ(source.path(), nullptr);
  EXPECT_EQ(hal.files, corrupt);
  auto foreign = completed;
  foreign.request.generation[0] ^= 1;
  EXPECT_FALSE(source.open(foreign));
  EXPECT_EQ(hal.files, corrupt);
}

TEST_F(HalCourseTransferTest, HistoricalCourseBaselineRefusesCorruptEvidenceAndIoFailuresWithoutWrites) {
  auto& hal = inventory_hal_test::state;
  const auto previous = declaration.manifest;
  removeInstalledCourse();
  ContentRemovalRecord completed;
  std::string receiptPath, planPath, cached;
  for (const auto& [path, record] : hal.files) {
    if (path.starts_with("/.crosspoint/companion/removal-done-") && decodeContentRemovalRecord(record, completed))
      receiptPath = path;
    if (path.starts_with("/.crosspoint/companion/removal-course-plan-")) planPath = path;
    if (path.starts_with(COURSE_REMOVAL_CACHE_PREFIX)) cached = path;
  }
  ASSERT_FALSE(receiptPath.empty());
  ASSERT_FALSE(planPath.empty());
  ASSERT_FALSE(cached.empty());
  const auto baseline = hal;
  HalHistoricalCourseBaseline source(
      generation, previous.logicalIdentity, scratch, [](void*) { return true; }, nullptr);
  ASSERT_TRUE(source.open(completed));
  ASSERT_TRUE(source.closeReaders());
  for (unsigned fault = 0; fault < 6; ++fault) {
    SCOPED_TRACE(fault);
    hal = baseline;
    switch (fault) {
      case 0:
        hal.files.at(planPath)[0] ^= 1;
        break;
      case 1:
        hal.files.at(receiptPath)[0] ^= 1;
        break;
      case 2:
        hal.failSyncPath = cached;
        break;
      case 3:
        hal.failClosePath = cached;
        break;
      case 4:
        hal.directoryErrorPath = TRANSFER_DIRECTORY;
        break;
      case 5:
        hal.files[CONTENT_REMOVAL_JOURNALS[0]] = {1};
        break;
    }
    const auto before = hal.files;
    EXPECT_FALSE(source.open(completed));
    EXPECT_EQ(source.path(), nullptr);
    EXPECT_EQ(source.manifest(), nullptr);
    EXPECT_EQ(hal.files, before);
  }
  hal = baseline;
  EXPECT_TRUE(source.open(completed));
  EXPECT_EQ(hal.files, baseline.files);
}

TEST_F(HalCourseTransferTest, OrdinaryUpdateRefusesIdentityConflictWithEarlierArchivedVersion) {
  addIdentityHistory();
  auto& hal = inventory_hal_test::state;
  hal.directories["/"] = {};
  const auto historical = declaration.manifest;
  const auto original = bytes;
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  HalCoursePackArchive archive(scratch, [](void*) { return true; }, nullptr);
  ASSERT_EQ(archive.publish(historical, ACTIVE_COURSE_PATH), CourseArchiveResult::Ok);
  ASSERT_TRUE(archive.closeReaders());
  bytes.back() ^= 1;
  bytes[12] ^= 1;
  sealPack();
  const auto active = bytes;
  hal.files[ACTIVE_COURSE_PATH] = active;
  std::array<uint8_t, COURSE_BINDING_SIZE> binding{};
  ASSERT_EQ(encodeCourseBinding(declaration.manifest, binding), binding.size());
  hal.files[COURSE_BINDING_PATH] = {binding.begin(), binding.end()};
  bytes[12] ^= 2;
  sealPack();
  hal.files["/candidate.pack"] = bytes;
  declaration.state.durableOffset = declaration.state.length;
  declaration.state.phase = TransferPhase::Receiving;
  EXPECT_FALSE(
      storage.validateContent(ACTIVE_COURSE_PATH, "/candidate.pack", declaration.manifest, declaration.state, scratch));
  EXPECT_EQ(hal.files.at(ACTIVE_COURSE_PATH), active);
  EXPECT_EQ(hal.files.at(COURSE_BINDING_PATH), std::vector<uint8_t>(binding.begin(), binding.end()));
  ASSERT_EQ(archive.open(historical.logicalIdentity, historical.contentHash), CourseArchiveResult::Ok);
  EXPECT_EQ(hal.files.at(archive.path()), original);
}

TEST_F(HalCourseTransferTest, OrdinaryUpdateRecoversOutgoingArchiveRenameFailuresBeforeReplacement) {
  addIdentityHistory();
  const auto original = bytes;
  const auto previous = declaration.manifest;
  std::array<uint8_t, COURSE_BINDING_SIZE> binding{};
  ASSERT_EQ(encodeCourseBinding(previous, binding), binding.size());
  bytes[12] ^= 1;
  sealPack();
  for (unsigned failedRename = 1; failedRename <= 2; ++failedRename) {
    for (const bool after : {false, true}) {
      SCOPED_TRACE(failedRename);
      SCOPED_TRACE(after);
      auto& hal = inventory_hal_test::state;
      hal = {};
      hal.enumerateFileMap = true;
      hal.files[ACTIVE_COURSE_PATH] = original;
      hal.files[COURSE_BINDING_PATH] = {binding.begin(), binding.end()};
      hal.files["/tinta/items.bin"] = {17};
      HalTransferStorage initial;
      {
        Transfer transfer(initial, scratch);
        receive(transfer);
        if (after)
          hal.failRenameAfter = hal.renames + failedRename;
        else
          hal.failRename = hal.renames + failedRename;
        EXPECT_NE(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::Ok);
        ASSERT_NE(transfer.current(), nullptr);
        EXPECT_EQ(transfer.current()->phase, TransferPhase::Receiving);
        EXPECT_EQ(hal.files.at(ACTIVE_COURSE_PATH), original);
        EXPECT_EQ(hal.files.at(COURSE_BINDING_PATH), std::vector<uint8_t>(binding.begin(), binding.end()));
        EXPECT_EQ(hal.files.at("/tinta/items.bin"), std::vector<uint8_t>({17}));
      }
      hal.failRename = hal.failRenameAfter = 0;
      HalTransferStorage resumed;
      Transfer transfer(resumed, scratch);
      ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
      ASSERT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::Ok);
      EXPECT_EQ(hal.files.at(ACTIVE_COURSE_PATH), bytes);
      EXPECT_EQ(hal.files.at("/tinta/items.bin"), std::vector<uint8_t>({17}));
      HalCoursePackArchive archive(scratch, [](void*) { return true; }, nullptr);
      ASSERT_EQ(archive.open(previous.logicalIdentity, previous.contentHash), CourseArchiveResult::Ok);
      EXPECT_EQ(hal.files.at(archive.path()), original);
    }
  }
}

TEST_F(HalCourseTransferTest, LegacyBridgeCannotHideConflictingArchivedIdentityHistory) {
  auto& hal = inventory_hal_test::state;
  hal.directories["/"] = {};
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  HalCoursePackArchive archive(scratch, [](void*) { return true; }, nullptr);
  ASSERT_EQ(archive.publish(declaration.manifest, ACTIVE_COURSE_PATH), CourseArchiveResult::Ok);
  ASSERT_TRUE(archive.closeReaders());
  addIdentityHistory();
  const auto bridgeBytes = bytes;
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  ASSERT_EQ(archive.publish(declaration.manifest, ACTIVE_COURSE_PATH), CourseArchiveResult::Ok);
  ASSERT_TRUE(archive.closeReaders());
  static constexpr char TRANSLATION[] = "house";
  auto text = std::search(bytes.begin(), bytes.end(), TRANSLATION, TRANSLATION + 5);
  ASSERT_NE(text, bytes.end());
  text[4] = 'E';
  sealPack();
  const auto candidate = declaration.manifest;
  hal.files["/candidate.pack"] = bytes;
  auto parser = makeUniqueNoThrow<tinta::core::pack::Pack>();
  ASSERT_TRUE(parser);
  HalCoursePackHistory history(scratch, [](void*) { return true; }, nullptr);
  HalCoursePackHistory bridges(scratch, [](void*) { return true; }, nullptr);
  HalCoursePackHistoryValidator validator(storage, *parser, scratch, &bridges);
  const auto before = hal.files;
  EXPECT_EQ(validator.validate(history, candidate, "/candidate.pack"), CourseHistoryResult::Ok);
  EXPECT_EQ(hal.files, before);
  bytes = bridgeBytes;
  bytes.back() ^= 1;
  bytes[12] ^= 2;
  sealPack();
  hal.files["/conflicting.pack"] = bytes;
  ASSERT_EQ(archive.publish(declaration.manifest, "/conflicting.pack"), CourseArchiveResult::Ok);
  ASSERT_TRUE(archive.closeReaders());
  const auto conflict = hal.files;
  EXPECT_EQ(validator.validate(history, candidate, "/candidate.pack"), CourseHistoryResult::Incompatible);
  EXPECT_EQ(hal.files, conflict);
}

TEST_F(HalCourseTransferTest, OrdinaryUpdateAdmitsBothHistoryReadersBeforeValidation) {
  addIdentityHistory();
  const auto original = bytes;
  std::array<uint8_t, COURSE_BINDING_SIZE> binding{};
  ASSERT_EQ(encodeCourseBinding(declaration.manifest, binding), binding.size());
  bytes[12] ^= 1;
  sealPack();
  for (unsigned shortage = 0; shortage < 2; ++shortage) {
    SCOPED_TRACE(shortage);
    auto& hal = inventory_hal_test::state;
    hal = {};
    hal.enumerateFileMap = true;
    hal.files[ACTIVE_COURSE_PATH] = original;
    hal.files[COURSE_BINDING_PATH] = {binding.begin(), binding.end()};
    hal.files["/tinta/items.bin"] = {17};
    HalTransferStorage initial;
    Transfer transfer(initial, scratch);
    receive(transfer);
    auto& heap = companion_memory_test::internal;
    if (shortage == 0)
      heap.freeBytes = 50 * 1024 + 2 * sizeof(HalCoursePackHistory);
    else
      heap.largestBlockBytes = sizeof(HalCoursePackHistory) - 1;
    EXPECT_NE(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::Ok);
    ASSERT_NE(transfer.current(), nullptr);
    EXPECT_EQ(transfer.current()->phase, TransferPhase::Receiving);
    EXPECT_EQ(hal.files.at(ACTIVE_COURSE_PATH), original);
    EXPECT_EQ(hal.files.at(COURSE_BINDING_PATH), std::vector<uint8_t>(binding.begin(), binding.end()));
    EXPECT_EQ(hal.files.at("/tinta/items.bin"), std::vector<uint8_t>({17}));
    heap = {1024 * 1024, 1024 * 1024, 1024 * 1024, 1024 * 1024};
    EXPECT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::Ok);
    EXPECT_EQ(hal.files.at(ACTIVE_COURSE_PATH), bytes);
    EXPECT_EQ(hal.files.at("/tinta/items.bin"), std::vector<uint8_t>({17}));
  }
}

TEST_F(HalCourseTransferTest, HistoricalCourseHistoryVisitsEveryRetainedRemovalVersionWithoutWrites) {
  auto& hal = inventory_hal_test::state;
  const auto previous = declaration.manifest;
  removeInstalledCourse(71);
  const auto first = hal.files;
  bytes[12] ^= 1;
  sealPack();
  const auto next = declaration.manifest;
  hal = {};
  hal.enumerateFileMap = true;
  removeInstalledCourse(72);
  hal.files.insert(first.begin(), first.end());
  const auto before = hal.files;
  struct Check {
    const ContentManifest* previous;
    const ContentManifest* next;
    unsigned oldCount = 0, newCount = 0;
  } check{&previous, &next};
  HalHistoricalCourseHistory history(
      generation, previous.logicalIdentity, scratch, [](void*) { return true; }, nullptr);
  EXPECT_EQ(history.visit(
                [](void* ctx, const ContentManifest& manifest, const char* path) {
                  auto& check = *static_cast<Check*>(ctx);
                  if (!path || !inventory_hal_test::state.files.contains(path)) return false;
                  if (manifest == *check.previous)
                    ++check.oldCount;
                  else if (manifest == *check.next)
                    ++check.newCount;
                  else
                    return false;
                  return true;
                },
                &check),
            HistoricalCourseHistoryResult::Ok);
  EXPECT_EQ(check.oldCount, 1u);
  EXPECT_EQ(check.newCount, 1u);
  EXPECT_EQ(hal.files, before);
  EXPECT_EQ(history.visit([](void*, const ContentManifest&, const char*) { return false; }, nullptr),
            HistoricalCourseHistoryResult::Incompatible);
  EXPECT_EQ(hal.files, before);
}

TEST_F(HalCourseTransferTest, HistoricalCourseHistoryRefusesStagesCorruptionAndIncompleteEnumerationBeforeVisits) {
  auto& hal = inventory_hal_test::state;
  const auto course = declaration.manifest.logicalIdentity;
  removeInstalledCourse();
  std::string receiptPath;
  for (const auto& [path, content] : hal.files)
    if (path.starts_with("/.crosspoint/companion/removal-done-")) receiptPath = path;
  ASSERT_FALSE(receiptPath.empty());
  const auto baseline = hal;
  unsigned visits = 0;
  auto visitor = [](void* ctx, const ContentManifest&, const char*) {
    ++*static_cast<unsigned*>(ctx);
    return true;
  };
  HalHistoricalCourseHistory history(generation, course, scratch, [](void*) { return true; }, nullptr);
  for (unsigned fault = 0; fault < 4; ++fault) {
    SCOPED_TRACE(fault);
    hal = baseline;
    if (fault == 0) hal.files[receiptPath + ".tmp"] = {1};
    if (fault == 1) hal.files.at(receiptPath)[0] ^= 1;
    if (fault == 2) hal.directoryErrorPath = TRANSFER_DIRECTORY;
    if (fault == 3) hal.files["/.crosspoint/companion/removal-done-bad"] = {1};
    const auto before = hal.files;
    const auto result = history.visit(visitor, &visits);
    EXPECT_NE(result, HistoricalCourseHistoryResult::Ok);
    EXPECT_NE(result, HistoricalCourseHistoryResult::Missing);
    EXPECT_EQ(visits, 0u);
    EXPECT_EQ(hal.files, before);
  }
  hal = baseline;
  EXPECT_EQ(history.visit(visitor, &visits), HistoricalCourseHistoryResult::Ok);
  EXPECT_EQ(visits, 1u);
}

TEST_F(HalCourseTransferTest, HistoricalCourseHistoryDistinguishesMissingCourseAndRejectsForeignGeneration) {
  auto& hal = inventory_hal_test::state;
  const auto course = declaration.manifest.logicalIdentity;
  removeInstalledCourse();
  const auto before = hal.files;
  unsigned visits = 0;
  auto visitor = [](void* ctx, const ContentManifest&, const char*) {
    ++*static_cast<unsigned*>(ctx);
    return true;
  };
  auto unknown = course;
  unknown[0] ^= 8;
  HalHistoricalCourseHistory missing(generation, unknown, scratch, [](void*) { return true; }, nullptr);
  EXPECT_EQ(missing.visit(visitor, &visits), HistoricalCourseHistoryResult::Missing);
  auto foreign = generation;
  foreign[0] ^= 2;
  HalHistoricalCourseHistory changedCard(foreign, course, scratch, [](void*) { return true; }, nullptr);
  EXPECT_EQ(changedCard.visit(visitor, &visits), HistoricalCourseHistoryResult::Corrupt);
  EXPECT_EQ(visits, 0u);
  EXPECT_EQ(hal.files, before);
}

TEST_F(HalCourseTransferTest, HistoricalCourseHistoryRejectsDuplicateReceiptsAndLostPermissionWithoutWrites) {
  auto& hal = inventory_hal_test::state;
  const auto course = declaration.manifest.logicalIdentity;
  removeInstalledCourse();
  std::string receiptPath;
  for (const auto& [path, content] : hal.files)
    if (path.starts_with("/.crosspoint/companion/removal-done-")) receiptPath = path;
  ASSERT_FALSE(receiptPath.empty());
  const auto baseline = hal.files;
  auto duplicate = receiptPath;
  duplicate[std::string(TRANSFER_DIRECTORY).size() + 1] = 'R';
  hal.files[duplicate] = hal.files.at(receiptPath);
  const auto conflicting = hal.files;
  bool allowed = true;
  unsigned visits = 0;
  HalHistoricalCourseHistory history(
      generation, course, scratch, [](void* ctx) { return *static_cast<bool*>(ctx); }, &allowed);
  EXPECT_EQ(history.visit(
                [](void* ctx, const ContentManifest&, const char*) {
                  ++*static_cast<unsigned*>(ctx);
                  return true;
                },
                &visits),
            HistoricalCourseHistoryResult::Corrupt);
  EXPECT_EQ(visits, 0u);
  EXPECT_EQ(hal.files, conflicting);
  hal.files = baseline;
  EXPECT_EQ(history.visit(
                [](void* ctx, const ContentManifest&, const char*) {
                  *static_cast<bool*>(ctx) = false;
                  return true;
                },
                &allowed),
            HistoricalCourseHistoryResult::Busy);
  EXPECT_EQ(hal.files, baseline);
  allowed = true;
  EXPECT_EQ(history.visit(
                [](void* ctx, const ContentManifest&, const char*) {
                  ++*static_cast<unsigned*>(ctx);
                  return true;
                },
                &visits),
            HistoricalCourseHistoryResult::Ok);
  EXPECT_EQ(visits, 1u);
  EXPECT_EQ(hal.files, baseline);
}

TEST_F(HalCourseTransferTest, NativeCourseReturnFindsLegacyBridgesAcrossArchivesAndRemovalReceipts) {
  const auto legacyBytes = bytes;
  const auto legacyDeclaration = declaration;
  auto& hal = inventory_hal_test::state;
  auto authorize = [&](HalTransferStorage& target, Transfer& transfer, const ContentManifest& previous) {
    CourseSwitchRequest consent;
    consent.generation = generation;
    consent.transaction = declaration.state.transaction;
    consent.previousCourse = previous.logicalIdentity;
    consent.previousHash = previous.contentHash;
    consent.nextCourse = declaration.manifest.logicalIdentity;
    consent.nextHash = declaration.manifest.contentHash;
    std::array<uint8_t, COURSE_SWITCH_REQUEST_SIZE> body{};
    std::array<uint8_t, COURSE_SWITCH_REPLY_SIZE> reply{};
    ASSERT_TRUE(encodeCourseSwitchRequest(consent, body));
    ASSERT_EQ(handleCourseSwitch(target, transfer, generation, declaration.state.owner, body, reply, scratch),
              reply.size());
    ASSERT_EQ(reply[0], static_cast<uint8_t>(TransferResult::Ok));
  };
  for (bool legacyInArchive : {false, true}) {
    SCOPED_TRACE(legacyInArchive);
    hal = {};
    hal.enumerateFileMap = true;
    hal.directories["/"] = {};
    hal.files["/tinta/items.bin"] = {17};
    declaration = legacyDeclaration;
    bytes = legacyBytes;
    HalTransferStorage initial;
    HalCoursePackArchive archive(scratch, [](void*) { return true; }, nullptr);
    if (legacyInArchive) {
      hal.files["/legacy.pack"] = bytes;
      ASSERT_EQ(archive.publish(declaration.manifest, "/legacy.pack"), CourseArchiveResult::Ok);
      ASSERT_TRUE(archive.closeReaders());
      addIdentityHistory();
      removeInstalledCourse();
    } else {
      removeInstalledCourse();
      addIdentityHistory();
      hal.files["/identity.pack"] = bytes;
      ASSERT_EQ(archive.publish(declaration.manifest, "/identity.pack"), CourseArchiveResult::Ok);
      ASSERT_TRUE(archive.closeReaders());
    }
    const auto identityBytes = bytes;
    const auto identityDeclaration = declaration;
    ContentManifest removed;
    ASSERT_TRUE(decodeCourseBinding(hal.files.at(COURSE_BINDING_PATH), removed));
    declaration.manifest.logicalIdentity[0] = 8;
    bytes[12] ^= 2;
    sealPack();
    {
      Transfer transfer(initial, scratch);
      receive(transfer, false);
      ASSERT_FALSE(HasFatalFailure());
      authorize(initial, transfer, removed);
      ASSERT_FALSE(HasFatalFailure());
      ASSERT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::Ok);
    }
    const auto current = declaration.manifest;
    const auto currentBytes = bytes;
    ASSERT_EQ(archive.open(removed.logicalIdentity, removed.contentHash), CourseArchiveResult::Ok);
    const std::string redundantCache = archive.path(), redundantReference = archive.referencePath();
    ASSERT_TRUE(archive.closeReaders());
    hal.files.erase(redundantCache);
    hal.files.erase(redundantCache + ".owner");
    hal.files.erase(redundantReference);
    char oldState[COURSE_STATE_PATH_SIZE], currentState[COURSE_STATE_PATH_SIZE];
    ASSERT_TRUE(courseStatePath(removed.logicalIdentity, "items.bin", oldState));
    ASSERT_TRUE(courseStatePath(current.logicalIdentity, "items.bin", currentState));
    hal.files[currentState] = {31};
    const auto baseline = hal;
    for (unsigned fault = 0; fault < 5; ++fault) {
      SCOPED_TRACE(fault);
      hal = baseline;
      declaration = identityDeclaration;
      bytes = identityBytes;
      if (fault == 1 || fault == 2) {
        bytes.back() ^= 1;
        sealPack();
        if (fault == 1) {
          hal.files["/conflicting.pack"] = bytes;
          ASSERT_EQ(archive.publish(declaration.manifest, "/conflicting.pack"), CourseArchiveResult::Ok);
          ASSERT_TRUE(archive.closeReaders());
        } else {
          hal = {};
          hal.enumerateFileMap = true;
          removeInstalledCourse(73);
          const auto conflicting = hal.files;
          hal = baseline;
          for (const auto& [path, data] : conflicting) {
            if (path.starts_with("/.crosspoint/companion/removal-done-") ||
                path.starts_with("/.crosspoint/companion/removal-course-plan-") ||
                path.starts_with("/.crosspoint/companion/course-removed-")) {
              hal.files.insert({path, data});
            }
          }
        }
      }
      std::string retainedCache;
      if (fault == 3 || fault == 4) {
        for (const auto& [path, data] : hal.files) {
          if (path.starts_with("/.crosspoint/companion/course-removed-")) retainedCache = path;
        }
        ASSERT_FALSE(retainedCache.empty());
      }
      declaration = identityDeclaration;
      declaration.state.transaction[0] = 5;
      bytes = identityBytes;
      static constexpr char TRANSLATION[] = "house";
      auto text = std::search(bytes.begin(), bytes.end(), TRANSLATION, TRANSLATION + 5);
      ASSERT_NE(text, bytes.end());
      text[4] = 'E';
      sealPack();
      HalTransferStorage reopened;
      Transfer transfer(reopened, scratch);
      receive(transfer, false);
      ASSERT_FALSE(HasFatalFailure());
      authorize(reopened, transfer, current);
      ASSERT_FALSE(HasFatalFailure());
      if (fault == 3) hal.readErrorPath = retainedCache;
      if (fault == 4) hal.failClosePath = retainedCache;
      const auto result = transfer.commit(declaration.state.transaction, declaration.state.owner);
      ContentManifest installed;
      ASSERT_TRUE(decodeCourseBinding(hal.files.at(COURSE_BINDING_PATH), installed));
      if (fault) {
        EXPECT_NE(result, TransferResult::Ok);
        EXPECT_EQ(installed, current);
        EXPECT_EQ(hal.files.at(ACTIVE_COURSE_PATH), currentBytes);
      } else {
        EXPECT_EQ(result, TransferResult::Ok);
        EXPECT_EQ(installed, declaration.manifest);
        EXPECT_EQ(hal.files.at(ACTIVE_COURSE_PATH), bytes);
      }
      EXPECT_EQ(hal.files.at(oldState), std::vector<uint8_t>({17}));
      EXPECT_EQ(hal.files.at(currentState), std::vector<uint8_t>({31}));
      if (fault == 3 || fault == 4) {
        ASSERT_NE(transfer.current(), nullptr);
        EXPECT_EQ(transfer.current()->phase, TransferPhase::Receiving);
        hal.readErrorPath.clear();
        hal.failClosePath.clear();
        EXPECT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::Ok);
        EXPECT_EQ(hal.files.at(ACTIVE_COURSE_PATH), bytes);
        EXPECT_EQ(hal.files.at(oldState), std::vector<uint8_t>({17}));
        EXPECT_EQ(hal.files.at(currentState), std::vector<uint8_t>({31}));
      }
    }
  }
}

TEST_F(HalCourseTransferTest, RemovedCourseUpdateRequiresHistoricalReaderHeapAdmissionAndRetries) {
  addIdentityHistory();
  auto& hal = inventory_hal_test::state;
  hal.files["/tinta/items.bin"] = {17};
  const auto previous = declaration.manifest;
  removeInstalledCourse();
  const auto binding = hal.files.at(COURSE_BINDING_PATH);
  const auto proof = hal.files.at(COURSE_REMOVAL_PROOF_PATH);
  bytes[12] ^= 1;
  sealPack();
  Transfer transfer(storage, scratch);
  receive(transfer, false);
  ASSERT_FALSE(HasFatalFailure());
  companion_memory_test::internal.largestBlockBytes = sizeof(HalHistoricalCourseHistory) - 1;
  EXPECT_NE(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::Ok);
  ASSERT_NE(transfer.current(), nullptr);
  EXPECT_EQ(transfer.current()->phase, TransferPhase::Receiving);
  EXPECT_FALSE(hal.files.contains(ACTIVE_COURSE_PATH));
  EXPECT_EQ(hal.files.at(COURSE_BINDING_PATH), binding);
  EXPECT_EQ(hal.files.at(COURSE_REMOVAL_PROOF_PATH), proof);
  char statePath[COURSE_STATE_PATH_SIZE];
  ASSERT_TRUE(courseStatePath(previous.logicalIdentity, "items.bin", statePath));
  EXPECT_EQ(hal.files.at(statePath), std::vector<uint8_t>({17}));
  companion_memory_test::internal = {1024 * 1024, 1024 * 1024, 1024 * 1024, 1024 * 1024};
  EXPECT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::Ok);
  EXPECT_EQ(hal.files.at(ACTIVE_COURSE_PATH), bytes);
  EXPECT_EQ(hal.files.at(statePath), std::vector<uint8_t>({17}));
}

TEST_F(HalCourseTransferTest, OrdinaryUpdateRejectsOlderRemovalIdentityDespiteCompatibleCurrentArchive) {
  addIdentityHistory();
  auto& hal = inventory_hal_test::state;
  hal.files["/tinta/items.bin"] = {17};
  removeInstalledCourse(71);
  const auto older = hal.files;
  bytes.back() ^= 1;
  bytes[12] ^= 1;
  sealPack();
  hal = {};
  hal.enumerateFileMap = true;
  hal.files["/tinta/items.bin"] = {17};
  removeInstalledCourse(72);
  hal.files.insert(older.begin(), older.end());
  const auto previous = declaration.manifest;
  const auto binding = hal.files.at(COURSE_BINDING_PATH);
  const auto proof = hal.files.at(COURSE_REMOVAL_PROOF_PATH);
  bytes[12] ^= 2;
  sealPack();
  Transfer transfer(storage, scratch);
  receive(transfer, false);
  ASSERT_FALSE(HasFatalFailure());
  EXPECT_NE(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::Ok);
  ASSERT_NE(transfer.current(), nullptr);
  EXPECT_EQ(transfer.current()->phase, TransferPhase::Receiving);
  EXPECT_FALSE(hal.files.contains(ACTIVE_COURSE_PATH));
  EXPECT_EQ(hal.files.at(COURSE_BINDING_PATH), binding);
  EXPECT_EQ(hal.files.at(COURSE_REMOVAL_PROOF_PATH), proof);
  char statePath[COURSE_STATE_PATH_SIZE];
  ASSERT_TRUE(courseStatePath(previous.logicalIdentity, "items.bin", statePath));
  EXPECT_EQ(hal.files.at(statePath), std::vector<uint8_t>({17}));
  for (const auto& [path, data] : older) {
    if (path != COURSE_BINDING_PATH && path != COURSE_REMOVAL_PROOF_PATH) {
      EXPECT_EQ(hal.files.at(path), data);
    }
  }
  hal.files["/candidate.pack"] = bytes;
  auto parser = makeUniqueNoThrow<tinta::core::pack::Pack>();
  ASSERT_TRUE(parser);
  HalCoursePackHistory history(scratch, [](void*) { return true; }, nullptr);
  HalCoursePackHistoryValidator validator(storage, *parser, scratch);
  EXPECT_EQ(validator.validate(history, declaration.manifest, "/candidate.pack"), CourseHistoryResult::Ok);
}

TEST_F(HalCourseTransferTest, HistoricalReceiptValidationUsesLegacyIdentityBridgeWithoutWrites) {
  auto& hal = inventory_hal_test::state;
  removeInstalledCourse(71);
  const auto legacy = hal.files;
  addIdentityHistory();
  hal = {};
  hal.enumerateFileMap = true;
  removeInstalledCourse(72);
  hal.files.insert(legacy.begin(), legacy.end());
  static constexpr char TRANSLATION[] = "house";
  auto text = std::search(bytes.begin(), bytes.end(), TRANSLATION, TRANSLATION + 5);
  ASSERT_NE(text, bytes.end());
  text[4] = 'E';
  sealPack();
  hal.files["/candidate.pack"] = bytes;
  const auto before = hal.files;
  auto parser = makeUniqueNoThrow<tinta::core::pack::Pack>();
  ASSERT_TRUE(parser);
  HalHistoricalCourseHistory history(
      generation, declaration.manifest.logicalIdentity, scratch, [](void*) { return true; }, nullptr);
  HalHistoricalCourseHistory bridges(
      generation, declaration.manifest.logicalIdentity, scratch, [](void*) { return true; }, nullptr);
  HalCoursePackHistoryValidator validator(storage, *parser, scratch, nullptr, &bridges);
  EXPECT_EQ(validator.validate(history, declaration.manifest, "/candidate.pack"), HistoricalCourseHistoryResult::Ok);
  EXPECT_EQ(hal.files, before);
  bytes.back() ^= 1;
  sealPack();
  hal.files["/candidate.pack"] = bytes;
  const auto conflicting = hal.files;
  EXPECT_EQ(validator.validate(history, declaration.manifest, "/candidate.pack"),
            HistoricalCourseHistoryResult::Incompatible);
  EXPECT_EQ(hal.files, conflicting);
}

TEST_F(HalCourseTransferTest, HistoricalReceiptValidationRejectsRetiredIdentityAndLanguageChanges) {
  addIdentityHistory();
  auto& hal = inventory_hal_test::state;
  removeInstalledCourse();
  const auto original = bytes;
  auto parser = makeUniqueNoThrow<tinta::core::pack::Pack>();
  ASSERT_TRUE(parser);
  HalHistoricalCourseHistory history(
      generation, declaration.manifest.logicalIdentity, scratch, [](void*) { return true; }, nullptr);
  HalCoursePackHistoryValidator validator(storage, *parser, scratch);
  for (unsigned change = 0; change < 2; ++change) {
    SCOPED_TRACE(change);
    bytes = original;
    if (change == 0)
      bytes.back() ^= 1;
    else
      bytes[offsetof(tinta::core::pack::Header, locale)] = 'f';
    sealPack();
    hal.files["/candidate.pack"] = bytes;
    const auto before = hal.files;
    EXPECT_EQ(validator.validate(history, declaration.manifest, "/candidate.pack"),
              HistoricalCourseHistoryResult::Incompatible);
    EXPECT_EQ(hal.files, before);
  }
}

TEST_F(HalCourseTransferTest, BaselineReviewCapturesUnboundGlobalFilesWithoutMigrationOrImportAuthority) {
  auto& hal = inventory_hal_test::state;
  hal.directories["/tinta"] = {};
  hal.directories[TRANSFER_DIRECTORY] = {};
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  hal.files["/tinta/items.bin"] = {17};
  hal.files["/tinta/usage.bin"] = {23};
  Identity nativeReader{};
  nativeReader[0] = 51;
  auto review = makeUniqueNoThrow<HalCourseBaselineReviewCapture>(scratch, [](void*) { return true; }, nullptr);
  ASSERT_TRUE(review);
  const auto before = hal.files;
  ASSERT_EQ(review->capture(nativeReader, generation, declaration.manifest.logicalIdentity),
            CourseBaselineReviewResult::Ok);
  const auto encoded = std::vector<uint8_t>(review->bytes().begin(), review->bytes().end());
  CourseBaselineReviewView view;
  EXPECT_FALSE(view.decode(encoded));
  ASSERT_TRUE(view.decode(encoded, true));
  EXPECT_FALSE(view.isolated());
  EXPECT_EQ(view.count(), 10u);
  EXPECT_EQ(encoded[4], 2);
  EXPECT_EQ(encoded[6], 1);
  EXPECT_EQ(hal.files, before);
  ASSERT_TRUE(review->closeReaders());
  auto store = makeUniqueNoThrow<HalCourseBaselineReviewStore>(
      std::span(scratch).subspan(COURSE_BASELINE_REVIEW_MAX_SIZE), [](void*) { return true; }, nullptr);
  Digest hash{};
  SHA256(encoded.data(), encoded.size(), hash.data());
  ASSERT_EQ(store->publish(encoded, hash), CourseBaselineReviewStoreResult::Ok);
  ASSERT_EQ(store->open(hash, nativeReader, generation, declaration.manifest.logicalIdentity,
                        std::span(scratch).first(COURSE_BASELINE_REVIEW_MAX_SIZE)),
            CourseBaselineReviewStoreResult::Ok);
  EXPECT_FALSE(view.decode(std::span(scratch).first(encoded.size())));
  EXPECT_TRUE(view.decode(std::span(scratch).first(encoded.size()), true));
  CourseBaselineReviewPageRequest request;
  request.generation = generation;
  request.course = declaration.manifest.logicalIdentity;
  std::vector<uint8_t> collected;
  collected.reserve(encoded.size());
  auto response = std::span(scratch).last(MAX_CONTROL_PAYLOAD);
  do {
    const auto length = handleHalCourseBaselineReviewRequest(
        *store, nativeReader, generation, request, [](void*) { return true; }, nullptr, scratch, response);
    ASSERT_GT(length, 0u);
    CourseBaselineReviewPageView page;
    ASSERT_TRUE(decodeCourseBaselineReviewPage(response.first(length), page));
    EXPECT_EQ(page.hash, hash);
    EXPECT_EQ(page.offset, collected.size());
    collected.insert(collected.end(), page.bytes.begin(), page.bytes.end());
    request.hash = hash;
    request.offset = collected.size();
  } while (collected.size() < encoded.size());
  EXPECT_EQ(collected, encoded);
  ASSERT_TRUE(store->closeReaders());
  store.reset();
  review.reset();
  CourseBaselineImportRequest approval;
  approval.generation = generation;
  approval.owner = declaration.state.owner;
  approval.transaction = declaration.state.transaction;
  approval.manifest = declaration.manifest;
  approval.reviewHash = hash;
  Transfer transfer(storage, std::span(scratch).subspan(TRANSFER_OFFSET));
  ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
  const auto preserved = hal.files;
  EXPECT_EQ(
      beginHalCourseBaselineImport(
          transfer, nativeReader, generation, approval.owner, approval, scratch, [](void*) { return true; }, nullptr),
      TransferResult::Unauthorized);
  EXPECT_EQ(transfer.current(), nullptr);
  EXPECT_EQ(hal.files, preserved);
}

TEST_F(HalCourseTransferTest, UnboundReviewRefusesPendingBindingMigrationAndUnknownDirectories) {
  auto& hal = inventory_hal_test::state;
  hal.directories["/tinta"] = {};
  hal.directories[TRANSFER_DIRECTORY] = {};
  hal.files["/tinta/items.bin"] = {17};
  Identity nativeReader{};
  nativeReader[0] = 51;
  auto review = makeUniqueNoThrow<HalCourseBaselineReviewCapture>(scratch, [](void*) { return true; }, nullptr);
  ASSERT_TRUE(review);
  for (const auto* marker :
       {COURSE_BINDING_PATH, COURSE_BINDING_STAGE, COURSE_BINDING_BACKUP, COURSE_STATE_MIGRATION_PATHS.intent,
        COURSE_STATE_MIGRATION_PATHS.stage, COURSE_STATE_MIGRATION_PATHS.done, COURSE_STATE_MIGRATION_PATHS.doneStage,
        COURSE_MARK_MIGRATION_PATHS.intent, COURSE_MARK_MIGRATION_PATHS.stage, COURSE_MARK_MIGRATION_PATHS.done,
        COURSE_MARK_MIGRATION_PATHS.doneStage}) {
    hal.files[marker] = {1};
    const auto before = hal.files;
    EXPECT_NE(review->capture(nativeReader, generation, declaration.manifest.logicalIdentity),
              CourseBaselineReviewResult::Ok);
    EXPECT_EQ(hal.files, before);
    hal.files.erase(marker);
  }
  hal.directories["/tinta/courses"] = {};
  const auto before = hal.files;
  EXPECT_NE(review->capture(nativeReader, generation, declaration.manifest.logicalIdentity),
            CourseBaselineReviewResult::Ok);
  EXPECT_EQ(hal.files, before);
}

TEST_F(HalCourseTransferTest, UnboundIntentEvidencePreventsReadingOrFreshReviewBeforeRecovery) {
  auto& hal = inventory_hal_test::state;
  hal.directories["/tinta"] = {};
  hal.directories[TRANSFER_DIRECTORY] = {};
  hal.files["/tinta/items.bin"] = {17};
  Identity nativeReader{};
  nativeReader.fill(51);
  Identity selected{};
  selected.fill(63);
  bool bound = false;
  auto review = makeUniqueNoThrow<HalCourseBaselineReviewCapture>(scratch, [](void*) { return true; }, nullptr);
  ASSERT_TRUE(review);
  for (const auto paths : {std::span(UNBOUND_COURSE_INTENT_PATHS), std::span(UNBOUND_COURSE_INTENT_STAGES)}) {
    for (const auto* path : paths) {
      for (const bool valid : {false, true}) {
        hal.files[path] = {1};
        if (valid) {
          UnboundCourseMigrationIntent intent;
          intent.reader = nativeReader;
          intent.request.original = {generation, declaration.state.owner, declaration.state.transaction,
                                     declaration.manifest, declaration.manifest.contentHash};
          intent.activePack = declaration.manifest;
          std::array<uint8_t, UNBOUND_COURSE_MIGRATION_INTENT_SIZE> encoded{};
          ASSERT_TRUE(encodeUnboundCourseMigrationIntent(intent, encoded));
          hal.files[path] = {encoded.begin(), encoded.end()};
        }
        const auto before = hal.files;
        const auto directories = hal.directories;
        EXPECT_FALSE(selectActiveCourseState(storage, scratch, selected, bound));
        EXPECT_FALSE(bound);
        EXPECT_EQ(selected[0], 63);
        EXPECT_NE(review->capture(nativeReader, generation, declaration.manifest.logicalIdentity),
                  CourseBaselineReviewResult::Ok);
        EXPECT_EQ(hal.files, before);
        EXPECT_EQ(hal.directories.size(), directories.size());
        for (const auto& [directory, entries] : directories) {
          ASSERT_TRUE(hal.directories.contains(directory));
          EXPECT_EQ(hal.directories.at(directory).size(), entries.size());
        }
        hal.files.erase(path);
      }
    }
  }
  ASSERT_TRUE(selectActiveCourseState(storage, scratch, selected, bound));
  EXPECT_FALSE(bound);
  EXPECT_EQ(selected, Identity{});
  ASSERT_EQ(review->capture(nativeReader, generation, declaration.manifest.logicalIdentity),
            CourseBaselineReviewResult::Ok);
}

TEST_F(HalCourseTransferTest, BaselineReviewCapturesFullIsolatedStateAndGlobalAuthorityWithoutWrites) {
  auto& hal = inventory_hal_test::state;
  hal.files["/tinta/items.bin"] = {17};
  hal.files["/tinta/starred.bin"] = {23};
  removeInstalledCourse();
  Identity nativeReader{};
  nativeReader[0] = 51;
  char state[COURSE_STATE_PATH_SIZE];
  ASSERT_TRUE(courseStatePath(declaration.manifest.logicalIdentity, "custom.bin", state));
  hal.files[state] = {31};
  auto review = makeUniqueNoThrow<HalCourseBaselineReviewCapture>(scratch, [](void*) { return true; }, nullptr);
  ASSERT_TRUE(review);
  const auto before = hal.files;
  ASSERT_EQ(review->capture(nativeReader, generation, declaration.manifest.logicalIdentity),
            CourseBaselineReviewResult::Ok);
  ASSERT_NE(review->hash(), nullptr);
  const auto legacyHash = *review->hash();
  const auto encoded = std::vector<uint8_t>(review->bytes().begin(), review->bytes().end());
  CourseBaselineReviewView view;
  ASSERT_TRUE(view.decode(encoded));
  EXPECT_EQ(view.count(), 10u);
  Digest actual{};
  SHA256(encoded.data(), encoded.size(), actual.data());
  EXPECT_EQ(actual, legacyHash);
  EXPECT_EQ(hal.files, before);
  ASSERT_EQ(review->capture(nativeReader, generation, declaration.manifest.logicalIdentity),
            CourseBaselineReviewResult::Ok);
  EXPECT_EQ(*review->hash(), legacyHash);
  EXPECT_EQ(std::vector<uint8_t>(review->bytes().begin(), review->bytes().end()), encoded);
  const auto ordinary = hal;
  for (auto& [directory, entries] : hal.directories) {
    entries.clear();
    entries.reserve(hal.files.size() + hal.directories.size());
    const auto prefix = directory == "/" ? directory : directory + "/";
    for (const auto& [path, data] : hal.files) {
      if (path.starts_with(prefix) && path.find('/', prefix.size()) == std::string::npos)
        entries.push_back({path.substr(prefix.size()), false});
    }
    for (const auto& [path, children] : hal.directories) {
      if (path != directory && path.starts_with(prefix) && path.find('/', prefix.size()) == std::string::npos)
        entries.push_back({path.substr(prefix.size()), true});
    }
    std::reverse(entries.begin(), entries.end());
  }
  hal.enumerateFileMap = false;
  ASSERT_EQ(review->capture(nativeReader, generation, declaration.manifest.logicalIdentity),
            CourseBaselineReviewResult::Ok);
  EXPECT_EQ(*review->hash(), legacyHash);
  hal = ordinary;
  auto upper = std::string(state);
  std::transform(upper.end() - 10, upper.end(), upper.end() - 10,
                 [](unsigned char value) { return value >= 'a' && value <= 'z' ? value - ('a' - 'A') : value; });
  hal.files[upper] = hal.files.at(state);
  hal.files.erase(state);
  ASSERT_EQ(review->capture(nativeReader, generation, declaration.manifest.logicalIdentity),
            CourseBaselineReviewResult::Ok);
  EXPECT_EQ(*review->hash(), legacyHash);
  hal = ordinary;
  hal.files[state][0] ^= 1;
  ASSERT_EQ(review->capture(nativeReader, generation, declaration.manifest.logicalIdentity),
            CourseBaselineReviewResult::Ok);
  EXPECT_NE(*review->hash(), legacyHash);
  hal.files[state][0] ^= 1;
  hal.directories[TINTA_JOURNAL_DIRECTORY] = {};
  hal.files[TINTA_JOURNAL_EVENTS] = {1, 2};
  hal.files[TINTA_JOURNAL_HEADER_A] = {3};
  ASSERT_EQ(review->capture(nativeReader, generation, declaration.manifest.logicalIdentity),
            CourseBaselineReviewResult::Ok);
  const auto authoritativeHash = *review->hash();
  EXPECT_NE(authoritativeHash, legacyHash);
  hal.files[TINTA_JOURNAL_EVENTS][0] ^= 1;
  ASSERT_EQ(review->capture(nativeReader, generation, declaration.manifest.logicalIdentity),
            CourseBaselineReviewResult::Ok);
  EXPECT_NE(*review->hash(), authoritativeHash);
  const auto saved = hal.files;
  const auto priorReaderHash = *review->hash();
  nativeReader[0] ^= 1;
  ASSERT_EQ(review->capture(nativeReader, generation, declaration.manifest.logicalIdentity),
            CourseBaselineReviewResult::Ok);
  EXPECT_EQ(hal.files, saved);
  EXPECT_NE(*review->hash(), priorReaderHash);
}

TEST_F(HalCourseTransferTest, BaselineReviewRejectsStagesAmbiguousNamesAndReaderFailuresWithoutWrites) {
  auto& hal = inventory_hal_test::state;
  hal.files["/tinta/items.bin"] = {17};
  removeInstalledCourse();
  const auto baseline = hal;
  char state[COURSE_STATE_PATH_SIZE], stage[COURSE_STATE_PATH_SIZE], directory[COURSE_STATE_DIRECTORY_SIZE];
  ASSERT_TRUE(courseStatePath(declaration.manifest.logicalIdentity, "items.bin", state));
  ASSERT_TRUE(courseStatePath(declaration.manifest.logicalIdentity, "items.bin.tmp", stage));
  ASSERT_TRUE(courseStateDirectory(declaration.manifest.logicalIdentity, directory));
  Identity nativeReader{};
  nativeReader[0] = 51;
  auto review = makeUniqueNoThrow<HalCourseBaselineReviewCapture>(scratch, [](void*) { return true; }, nullptr);
  ASSERT_TRUE(review);
  for (unsigned fault = 0; fault < 7; ++fault) {
    SCOPED_TRACE(fault);
    hal = baseline;
    if (fault == 0) hal.files[stage] = {1};
    if (fault == 1) {
      auto duplicate = std::string(state);
      duplicate[duplicate.size() - 9] = 'I';
      hal.files[duplicate] = {1};
    }
    if (fault == 2) hal.directoryErrorPath = directory;
    if (fault == 3) hal.readErrorPath = state;
    if (fault == 4) hal.failClosePath = state;
    if (fault == 5) hal.failSyncPath = state;
    if (fault == 6) hal.files[COURSE_STATE_MIGRATION_DONE][0] ^= 1;
    const auto before = hal.files;
    const auto result = review->capture(nativeReader, generation, declaration.manifest.logicalIdentity);
    EXPECT_NE(result, CourseBaselineReviewResult::Ok);
    EXPECT_EQ(review->hash(), nullptr);
    EXPECT_TRUE(review->bytes().empty());
    EXPECT_EQ(hal.files, before);
  }
  hal = baseline;
  EXPECT_EQ(review->capture(nativeReader, generation, declaration.manifest.logicalIdentity),
            CourseBaselineReviewResult::Ok);
}

TEST_F(HalCourseTransferTest, BaselineReviewRefusesIncompleteJournalAndLatchesLoanPermissionLoss) {
  auto& hal = inventory_hal_test::state;
  hal.files["/tinta/items.bin"] = {17};
  removeInstalledCourse();
  hal.directories[TINTA_JOURNAL_DIRECTORY] = {};
  hal.files[TINTA_JOURNAL_EVENTS] = {1};
  Identity nativeReader{};
  nativeReader[0] = 51;
  bool permitted = true;
  auto review = makeUniqueNoThrow<HalCourseBaselineReviewCapture>(
      scratch, [](void* context) { return *static_cast<bool*>(context); }, &permitted);
  ASSERT_TRUE(review);
  const auto before = hal.files;
  EXPECT_EQ(review->capture(nativeReader, generation, declaration.manifest.logicalIdentity),
            CourseBaselineReviewResult::Corrupt);
  EXPECT_EQ(hal.files, before);
  hal.files[TINTA_JOURNAL_HEADER_B] = {2};
  ASSERT_EQ(review->capture(nativeReader, generation, declaration.manifest.logicalIdentity),
            CourseBaselineReviewResult::Ok);
  ASSERT_NE(review->hash(), nullptr);
  permitted = false;
  EXPECT_EQ(review->hash(), nullptr);
  EXPECT_TRUE(review->bytes().empty());
  permitted = true;
  EXPECT_EQ(review->hash(), nullptr);
  EXPECT_TRUE(review->bytes().empty());
  EXPECT_EQ(review->capture(nativeReader, generation, declaration.manifest.logicalIdentity),
            CourseBaselineReviewResult::Ok);
  EXPECT_TRUE(review->closeReaders());
  EXPECT_EQ(review->hash(), nullptr);
}

TEST_F(HalCourseTransferTest, BaselineReviewRejectsOversizedCohortsWithoutTruncatingEvidence) {
  auto& hal = inventory_hal_test::state;
  hal.files["/tinta/items.bin"] = {17};
  removeInstalledCourse();
  char path[COURSE_STATE_PATH_SIZE], name[24];
  for (unsigned at = 0; at < 57; ++at) {
    snprintf(name, sizeof(name), "state%02u.bin", at);
    ASSERT_TRUE(courseStatePath(declaration.manifest.logicalIdentity, name, path));
    hal.files[path] = {static_cast<uint8_t>(at)};
  }
  const auto before = hal.files;
  Identity nativeReader{};
  nativeReader[0] = 51;
  auto review = makeUniqueNoThrow<HalCourseBaselineReviewCapture>(scratch, [](void*) { return true; }, nullptr);
  ASSERT_TRUE(review);
  EXPECT_EQ(review->capture(nativeReader, generation, declaration.manifest.logicalIdentity),
            CourseBaselineReviewResult::TooManyFiles);
  EXPECT_TRUE(review->bytes().empty());
  EXPECT_EQ(review->hash(), nullptr);
  EXPECT_EQ(hal.files, before);
}

TEST_F(HalCourseTransferTest, SealedBaselineReviewRoundTripsAndRepeatedPublicationDoesNotWrite) {
  std::vector<uint8_t> encoded;
  Digest hash{};
  Identity reader{};
  prepareBaselineReview(encoded, hash, reader);
  ASSERT_FALSE(HasFatalFailure());
  auto& hal = inventory_hal_test::state;
  const auto before = hal.files;
  auto store = makeUniqueNoThrow<HalCourseBaselineReviewStore>(
      std::span(scratch).subspan(COURSE_BASELINE_REVIEW_MAX_SIZE), [](void*) { return true; }, nullptr);
  ASSERT_TRUE(store);
  ASSERT_EQ(store->publish(encoded, hash), CourseBaselineReviewStoreResult::Ok);
  ASSERT_NE(store->path(), nullptr);
  const std::string path = store->path();
  EXPECT_EQ(hal.files.at(path), encoded);
  EXPECT_EQ(hal.files.size(), before.size() + 1);
  for (const auto& [name, bytes] : before) EXPECT_EQ(hal.files.at(name), bytes);
  const auto saved = hal.files;
  const auto renames = hal.renames;
  ASSERT_EQ(store->publish(encoded, hash), CourseBaselineReviewStoreResult::Ok);
  EXPECT_EQ(hal.files, saved);
  EXPECT_EQ(hal.renames, renames);
  ASSERT_EQ(store->open(hash, reader, generation, declaration.manifest.logicalIdentity,
                        std::span(scratch).first(COURSE_BASELINE_REVIEW_MAX_SIZE)),
            CourseBaselineReviewStoreResult::Ok);
  EXPECT_EQ(hal.files.at(store->path()), encoded);
  EXPECT_TRUE(std::equal(encoded.begin(), encoded.end(), scratch.begin()));
}

TEST_F(HalCourseTransferTest, SealedBaselineReviewRecoversBothRenameBoundariesAndMatchingPrefixes) {
  std::vector<uint8_t> encoded;
  Digest hash{};
  Identity reader{};
  prepareBaselineReview(encoded, hash, reader);
  ASSERT_FALSE(HasFatalFailure());
  auto& hal = inventory_hal_test::state;
  const auto baseline = hal;
  auto store = makeUniqueNoThrow<HalCourseBaselineReviewStore>(
      std::span(scratch).subspan(COURSE_BASELINE_REVIEW_MAX_SIZE), [](void*) { return true; }, nullptr);
  ASSERT_TRUE(store);
  ASSERT_EQ(store->publish(encoded, hash), CourseBaselineReviewStoreResult::Ok);
  const std::string path = store->path();
  for (bool after : {false, true}) {
    hal = baseline;
    if (after)
      hal.failRenameAfter = hal.renames + 1;
    else
      hal.failRename = hal.renames + 1;
    EXPECT_EQ(store->publish(encoded, hash), CourseBaselineReviewStoreResult::IoError);
    hal.failRename = hal.failRenameAfter = 0;
    EXPECT_EQ(store->publish(encoded, hash), CourseBaselineReviewStoreResult::Ok);
    EXPECT_EQ(hal.files.at(path), encoded);
  }
  for (size_t length : {size_t{0}, size_t{1}, encoded.size() / 2, encoded.size()}) {
    SCOPED_TRACE(length);
    hal = baseline;
    hal.files[path + ".tmp"] = {encoded.begin(), encoded.begin() + length};
    ASSERT_EQ(store->publish(encoded, hash), CourseBaselineReviewStoreResult::Ok);
    EXPECT_EQ(hal.files.at(path), encoded);
    EXPECT_FALSE(hal.files.contains(path + ".tmp"));
    for (const auto& [name, data] : baseline.files) EXPECT_EQ(hal.files.at(name), data);
  }
}

TEST_F(HalCourseTransferTest, SealedBaselineReviewPreservesForeignCorruptAndDuplicateFiles) {
  std::vector<uint8_t> encoded;
  Digest hash{};
  Identity reader{};
  prepareBaselineReview(encoded, hash, reader);
  ASSERT_FALSE(HasFatalFailure());
  auto& hal = inventory_hal_test::state;
  const auto baseline = hal;
  auto store = makeUniqueNoThrow<HalCourseBaselineReviewStore>(
      std::span(scratch).subspan(COURSE_BASELINE_REVIEW_MAX_SIZE), [](void*) { return true; }, nullptr);
  ASSERT_TRUE(store);
  ASSERT_EQ(store->publish(encoded, hash), CourseBaselineReviewStoreResult::Ok);
  const std::string path = store->path();
  for (unsigned fault = 0; fault < 5; ++fault) {
    SCOPED_TRACE(fault);
    hal = baseline;
    if (fault == 0) {
      hal.files[path] = encoded;
      hal.files[path].back() ^= 1;
    }
    if (fault == 1) {
      hal.files[path + ".tmp"] = {encoded.begin(), encoded.begin() + 16};
      hal.files[path + ".tmp"][0] ^= 1;
    }
    if (fault == 2) {
      hal.files[path + ".tmp"] = encoded;
      hal.files[path + ".tmp"].push_back(0);
    }
    if (fault == 3) {
      hal.files[path] = encoded;
      hal.files[path + ".tmp"] = encoded;
    }
    if (fault == 4) {
      hal.files[path] = encoded;
      auto duplicate = path;
      duplicate[std::string(TRANSFER_DIRECTORY).size() + 1] = 'C';
      hal.files[duplicate] = encoded;
    }
    const auto before = hal.files;
    EXPECT_NE(store->publish(encoded, hash), CourseBaselineReviewStoreResult::Ok);
    EXPECT_EQ(store->path(), nullptr);
    EXPECT_EQ(hal.files, before);
  }
}

TEST_F(HalCourseTransferTest, NativeBaselineReviewPagesRequireVerifiedImmutableSourceAndExclusiveLoan) {
  std::vector<uint8_t> encoded;
  Digest hash{};
  Identity reader{};
  prepareBaselineReview(encoded, hash, reader);
  ASSERT_FALSE(HasFatalFailure());
  bool allowed = true;
  auto permission = [](void* context) { return *static_cast<bool*>(context); };
  auto store = makeUniqueNoThrow<HalCourseBaselineReviewStore>(
      std::span(scratch).subspan(COURSE_BASELINE_REVIEW_MAX_SIZE), permission, &allowed);
  ASSERT_TRUE(store);
  ASSERT_EQ(store->publish(encoded, hash), CourseBaselineReviewStoreResult::Ok);
  const std::string path = store->path();
  const auto retained = inventory_hal_test::state.files;
  auto response = std::span(scratch).last(MAX_CONTROL_PAYLOAD);
  std::vector<uint8_t> collected(encoded.size());
  size_t offset = 0;
  while (offset < encoded.size()) {
    const auto size =
        readHalCourseBaselineReviewPage(*store, permission, &allowed, hash, reader, generation,
                                        declaration.manifest.logicalIdentity, offset, 97, scratch, response);
    ASSERT_NE(size, 0u);
    CourseBaselineReviewPageView page;
    ASSERT_TRUE(decodeCourseBaselineReviewPage(response.first(size), page));
    EXPECT_EQ(page.hash, hash);
    EXPECT_EQ(page.offset, offset);
    std::copy(page.bytes.begin(), page.bytes.end(), collected.begin() + offset);
    offset += page.bytes.size();
  }
  EXPECT_EQ(collected, encoded);
  EXPECT_EQ(inventory_hal_test::state.files, retained);
  allowed = false;
  EXPECT_EQ(readHalCourseBaselineReviewPage(*store, permission, &allowed, hash, reader, generation,
                                            declaration.manifest.logicalIdentity, 0, 97, scratch, response),
            0u);
  allowed = true;
  EXPECT_EQ(readHalCourseBaselineReviewPage(*store, permission, &allowed, hash, reader, generation,
                                            declaration.manifest.logicalIdentity, 0, 97, scratch,
                                            std::span(scratch).first(MAX_CONTROL_PAYLOAD)),
            0u);
  auto foreign = reader;
  foreign[0] ^= 0x80;
  EXPECT_EQ(readHalCourseBaselineReviewPage(*store, permission, &allowed, hash, foreign, generation,
                                            declaration.manifest.logicalIdentity, 0, 97, scratch, response),
            0u);
  inventory_hal_test::state.files[path].back() ^= 1;
  EXPECT_EQ(readHalCourseBaselineReviewPage(*store, permission, &allowed, hash, reader, generation,
                                            declaration.manifest.logicalIdentity, 0, 97, scratch, response),
            0u);
  EXPECT_EQ(inventory_hal_test::state.files[path].back(), uint8_t(retained.at(path).back() ^ 1));
}

TEST_F(HalCourseTransferTest, NativeReviewRequestSealsCaptureAndResumesTheNamedFrozenRoster) {
  std::vector<uint8_t> encoded;
  Digest expected{};
  Identity reader{};
  prepareBaselineReview(encoded, expected, reader);
  ASSERT_FALSE(HasFatalFailure());
  const auto retained = inventory_hal_test::state.files;
  CourseBaselineReviewPageRequest request;
  request.generation = generation;
  request.course = declaration.manifest.logicalIdentity;
  request.limit = 97;
  bool allowed = true;
  auto permission = [](void* context) { return *static_cast<bool*>(context); };
  auto store = makeUniqueNoThrow<HalCourseBaselineReviewStore>(
      std::span(scratch).subspan(COURSE_BASELINE_REVIEW_MAX_SIZE), permission, &allowed);
  ASSERT_TRUE(store);
  auto response = std::span(scratch).last(MAX_CONTROL_PAYLOAD);
  const auto size = handleHalCourseBaselineReviewRequest(*store, reader, generation, request, permission, &allowed,
                                                         scratch, response);
  ASSERT_NE(size, 0u);
  CourseBaselineReviewPageView first;
  ASSERT_TRUE(decodeCourseBaselineReviewPage(response.first(size), first));
  EXPECT_EQ(first.hash, expected);
  EXPECT_EQ(first.offset, 0u);
  for (const auto& [path, data] : retained) EXPECT_EQ(inventory_hal_test::state.files.at(path), data);
  const auto sealed = inventory_hal_test::state.files;
  EXPECT_EQ(sealed.size(), retained.size() + 1);
  ASSERT_EQ(handleHalCourseBaselineReviewRequest(*store, reader, generation, request, permission, &allowed, scratch,
                                                 response),
            size);
  EXPECT_EQ(inventory_hal_test::state.files, sealed);
  std::array<char, COURSE_STATE_PATH_SIZE> learner{};
  ASSERT_TRUE(courseStatePath(request.course, "items.bin", learner));
  inventory_hal_test::state.files[learner.data()] = {19};
  request.hash = expected;
  request.offset = 97;
  const auto changed = inventory_hal_test::state.files;
  const auto nextSize = handleHalCourseBaselineReviewRequest(*store, reader, generation, request, permission, &allowed,
                                                             scratch, response);
  ASSERT_NE(nextSize, 0u);
  CourseBaselineReviewPageView next;
  ASSERT_TRUE(decodeCourseBaselineReviewPage(response.first(nextSize), next));
  EXPECT_EQ(next.hash, expected);
  EXPECT_EQ(next.offset, 97u);
  EXPECT_TRUE(std::equal(next.bytes.begin(), next.bytes.end(), encoded.begin() + 97));
  EXPECT_EQ(inventory_hal_test::state.files, changed);
}

TEST_F(HalCourseTransferTest, NativeReviewRequestRefusesInvalidContextAndLowHeapBeforeSealing) {
  std::vector<uint8_t> encoded;
  Digest hash{};
  Identity reader{};
  prepareBaselineReview(encoded, hash, reader);
  ASSERT_FALSE(HasFatalFailure());
  const auto retained = inventory_hal_test::state.files;
  CourseBaselineReviewPageRequest request;
  request.generation = generation;
  request.course = declaration.manifest.logicalIdentity;
  bool allowed = false;
  auto permission = [](void* context) { return *static_cast<bool*>(context); };
  auto store = makeUniqueNoThrow<HalCourseBaselineReviewStore>(
      std::span(scratch).subspan(COURSE_BASELINE_REVIEW_MAX_SIZE), permission, &allowed);
  ASSERT_TRUE(store);
  auto response = std::span(scratch).last(MAX_CONTROL_PAYLOAD);
  EXPECT_EQ(handleHalCourseBaselineReviewRequest(*store, reader, generation, request, permission, &allowed, scratch,
                                                 response),
            0u);
  allowed = true;
  auto foreign = generation;
  foreign[0] ^= 0x80;
  EXPECT_EQ(
      handleHalCourseBaselineReviewRequest(*store, reader, foreign, request, permission, &allowed, scratch, response),
      0u);
  EXPECT_EQ(handleHalCourseBaselineReviewRequest(*store, reader, generation, request, permission, &allowed, scratch,
                                                 std::span(scratch).first(MAX_CONTROL_PAYLOAD)),
            0u);
  const auto memory = companion_memory_test::internal;
  companion_memory_test::internal.freeBytes = 50 * 1024 + sizeof(HalCourseBaselineReviewCapture);
  EXPECT_EQ(handleHalCourseBaselineReviewRequest(*store, reader, generation, request, permission, &allowed, scratch,
                                                 response),
            0u);
  companion_memory_test::internal = memory;
  EXPECT_EQ(handleHalCourseBaselineReviewRequest(*store, reader, generation, request, permission, &allowed, scratch,
                                                 response.first(COURSE_BASELINE_REVIEW_PAGE_OVERHEAD)),
            0u);
  request.offset = 1;
  EXPECT_EQ(handleHalCourseBaselineReviewRequest(*store, reader, generation, request, permission, &allowed, scratch,
                                                 response),
            0u);
  EXPECT_EQ(inventory_hal_test::state.files, retained);
}

TEST_F(HalCourseTransferTest, SealedBaselineReviewRejectsForeignContextAndCorruptLoads) {
  std::vector<uint8_t> encoded;
  Digest hash{};
  Identity reader{};
  prepareBaselineReview(encoded, hash, reader);
  ASSERT_FALSE(HasFatalFailure());
  auto& hal = inventory_hal_test::state;
  auto store = makeUniqueNoThrow<HalCourseBaselineReviewStore>(
      std::span(scratch).subspan(COURSE_BASELINE_REVIEW_MAX_SIZE), [](void*) { return true; }, nullptr);
  ASSERT_TRUE(store);
  ASSERT_EQ(store->publish(encoded, hash), CourseBaselineReviewStoreResult::Ok);
  const std::string path = store->path();
  const auto baseline = hal;
  for (unsigned fault = 0; fault < 10; ++fault) {
    SCOPED_TRACE(fault);
    hal = baseline;
    auto expectedReader = reader, expectedGeneration = generation,
         expectedCourse = declaration.manifest.logicalIdentity;
    if (fault == 0) expectedReader[0] ^= 1;
    if (fault == 1) expectedGeneration[0] ^= 1;
    if (fault == 2) expectedCourse[0] ^= 1;
    if (fault == 3) hal.files[path].back() ^= 1;
    if (fault == 4) hal.files[path + ".tmp"] = {1};
    if (fault == 5) hal.readErrorPath = path;
    if (fault == 6) hal.failSyncPath = path;
    if (fault == 7) hal.failClosePath = path;
    if (fault == 8) hal.statErrorPath = path;
    if (fault == 9) hal.files.erase(path);
    const auto before = hal.files;
    EXPECT_NE(store->open(hash, expectedReader, expectedGeneration, expectedCourse,
                          std::span(scratch).first(COURSE_BASELINE_REVIEW_MAX_SIZE)),
              CourseBaselineReviewStoreResult::Ok);
    EXPECT_EQ(store->path(), nullptr);
    EXPECT_EQ(hal.files, before);
  }
  hal = baseline;
  struct AliasedContext {
    std::array<uint8_t, 8> prefix{};
    Identity reader{}, generation{}, course{};
    std::array<uint8_t, COURSE_BASELINE_REVIEW_MAX_SIZE - 56> rest{};
  } aliased;
  static_assert(sizeof(aliased) == COURSE_BASELINE_REVIEW_MAX_SIZE);
  aliased.reader = reader;
  aliased.reader[0] ^= 1;
  EXPECT_EQ(store->open(hash, aliased.reader, generation, declaration.manifest.logicalIdentity,
                        {reinterpret_cast<uint8_t*>(&aliased), sizeof(aliased)}),
            CourseBaselineReviewStoreResult::Corrupt);
}

TEST_F(HalCourseTransferTest, SealedBaselineReviewPublicationRetriesIoFailuresAndRevokesLoans) {
  std::vector<uint8_t> encoded;
  Digest hash{};
  Identity reader{};
  prepareBaselineReview(encoded, hash, reader);
  ASSERT_FALSE(HasFatalFailure());
  auto& hal = inventory_hal_test::state;
  bool permitted = true;
  auto store = makeUniqueNoThrow<HalCourseBaselineReviewStore>(
      std::span(scratch).subspan(COURSE_BASELINE_REVIEW_MAX_SIZE),
      [](void* context) { return *static_cast<bool*>(context); }, &permitted);
  ASSERT_TRUE(store);
  ASSERT_EQ(store->publish(encoded, hash), CourseBaselineReviewStoreResult::Ok);
  const std::string path = store->path();
  hal.files.erase(path);
  const auto baseline = hal;
  for (unsigned fault = 0; fault < 3; ++fault) {
    hal = baseline;
    if (fault == 0) {
      hal.failWritePath = path + ".tmp";
      hal.failMatchingWrite = 1;
      hal.matchingWrites = 0;
    }
    if (fault == 1) hal.failSyncPath = path + ".tmp";
    if (fault == 2) hal.failClosePath = path + ".tmp";
    EXPECT_NE(store->publish(encoded, hash), CourseBaselineReviewStoreResult::Ok);
    EXPECT_EQ(store->path(), nullptr);
    hal.failWritePath.clear();
    hal.failSyncPath.clear();
    hal.failClosePath.clear();
    ASSERT_EQ(store->publish(encoded, hash), CourseBaselineReviewStoreResult::Ok);
    EXPECT_EQ(hal.files.at(path), encoded);
    for (const auto& [name, data] : baseline.files) EXPECT_EQ(hal.files.at(name), data);
  }
  permitted = false;
  EXPECT_EQ(store->path(), nullptr);
  permitted = true;
  EXPECT_EQ(store->path(), nullptr);
  EXPECT_EQ(store->publish(encoded, hash), CourseBaselineReviewStoreResult::Ok);
  EXPECT_TRUE(store->closeReaders());
  EXPECT_EQ(store->path(), nullptr);
}

TEST_F(HalCourseTransferTest, ReviewedBaselineBackupsPreserveEveryPresentFileAndRepeatWithoutWriting) {
  std::vector<uint8_t> encoded;
  Digest hash{};
  Identity reader{};
  prepareBaselineReview(encoded, hash, reader);
  ASSERT_FALSE(HasFatalFailure());
  auto& hal = inventory_hal_test::state;
  auto store = makeUniqueNoThrow<HalCourseBaselineReviewStore>(
      std::span(scratch).subspan(COURSE_BASELINE_REVIEW_MAX_SIZE), [](void*) { return true; }, nullptr);
  ASSERT_TRUE(store);
  ASSERT_EQ(store->publish(encoded, hash), CourseBaselineReviewStoreResult::Ok);
  std::string prefix = store->path();
  prefix.replace(prefix.find("course-review-"), strlen("course-review-"), "course-review-state-");
  store.reset();
  const auto originals = hal.files;
  bool permitted = true;
  auto backups = makeUniqueNoThrow<HalCourseBaselineReviewBackup>(
      scratch, [](void* context) { return *static_cast<bool*>(context); }, &permitted);
  ASSERT_TRUE(backups);
  ASSERT_TRUE(backups->preserve(hash, reader, generation, declaration.manifest.logicalIdentity));
  ASSERT_TRUE(backups->complete());
  CourseBaselineReviewView view;
  ASSERT_TRUE(view.decode(encoded));
  size_t present = 0;
  for (size_t i = 0; i < view.count(); ++i) {
    char suffix[8]{};
    snprintf(suffix, sizeof(suffix), "-%02x", static_cast<unsigned>(i));
    const auto path = prefix + suffix;
    const auto entry = view.entry(i);
    EXPECT_FALSE(hal.files.contains(path + ".tmp"));
    if (!entry[1]) {
      EXPECT_FALSE(hal.files.contains(path));
      continue;
    }
    ++present;
    ASSERT_TRUE(hal.files.contains(path));
    const auto& copy = hal.files.at(path);
    EXPECT_EQ(copy.size(), course_review_detail::number(entry, 28, 8));
    Digest actual{};
    SHA256(copy.data(), copy.size(), actual.data());
    EXPECT_TRUE(std::equal(actual.begin(), actual.end(), entry.begin() + 36));
  }
  EXPECT_EQ(hal.files.size(), originals.size() + present);
  for (const auto& [path, bytes] : originals) EXPECT_EQ(hal.files.at(path), bytes);
  const auto completeFiles = hal.files;
  const auto renames = hal.renames;
  ASSERT_TRUE(backups->preserve(hash, reader, generation, declaration.manifest.logicalIdentity));
  EXPECT_EQ(hal.files, completeFiles);
  EXPECT_EQ(hal.renames, renames);
  permitted = false;
  EXPECT_FALSE(backups->complete());
  permitted = true;
  EXPECT_FALSE(backups->complete());
  ASSERT_TRUE(backups->preserve(hash, reader, generation, declaration.manifest.logicalIdentity));
  EXPECT_TRUE(backups->closeReaders());
  EXPECT_FALSE(backups->complete());
}

TEST_F(HalCourseTransferTest, UnboundBackupsPreserveGlobalSpellingAndDiagnosticsWithoutOfferingIsolationLoan) {
  Digest hash{};
  Identity reader{};
  const auto prefix = prepareUnboundBackupReview(hash, reader);
  ASSERT_FALSE(prefix.empty());
  auto& hal = inventory_hal_test::state;
  const auto originals = hal.files;
  bool allowed = true;
  auto backups = makeUniqueNoThrow<HalCourseBaselineReviewBackup>(
      scratch, [](void* p) { return *static_cast<bool*>(p); }, &allowed);
  ASSERT_TRUE(backups);
  EXPECT_FALSE(backups->preserve(hash, reader, generation, declaration.manifest.logicalIdentity));
  EXPECT_FALSE(backups->verifyCurrent(hash, reader, generation, declaration.manifest.logicalIdentity));
  EXPECT_EQ(hal.files, originals);
  ASSERT_TRUE(backups->preserveUnbound(hash, reader, generation, declaration.manifest.logicalIdentity));
  EXPECT_TRUE(backups->unboundComplete());
  EXPECT_FALSE(backups->complete());
  EXPECT_EQ(hal.files.at(prefix + "-00"), originals.at("/tinta/ITEMS.BIN"));
  EXPECT_EQ(hal.files.at(prefix + "-01"), originals.at("/tinta/usage.bin"));
  for (const auto& [path, data] : originals) EXPECT_EQ(hal.files.at(path), data);
  const auto preserved = hal.files;
  const auto renames = hal.renames;
  ASSERT_TRUE(backups->preserveUnbound(hash, reader, generation, declaration.manifest.logicalIdentity));
  EXPECT_EQ(hal.files, preserved);
  EXPECT_EQ(hal.renames, renames);
  allowed = false;
  EXPECT_FALSE(backups->unboundComplete());
  allowed = true;
  EXPECT_FALSE(backups->unboundComplete());
  ASSERT_TRUE(backups->verifyStoredUnbound(hash, reader, generation, declaration.manifest.logicalIdentity));
  EXPECT_FALSE(backups->unboundComplete());
  EXPECT_FALSE(backups->verifyStored(hash, reader, generation, declaration.manifest.logicalIdentity));
  hal.files.erase("/tinta/ITEMS.BIN");
  ASSERT_TRUE(backups->verifyStoredUnbound(hash, reader, generation, declaration.manifest.logicalIdentity));
  EXPECT_FALSE(backups->preserveUnbound(hash, reader, generation, declaration.manifest.logicalIdentity));
  EXPECT_EQ(hal.files.at(prefix + "-00"), originals.at("/tinta/ITEMS.BIN"));
}

TEST_F(HalCourseTransferTest, UnboundBackupsResumeOwnedPartialCopiesAndRefuseConflictingEvidence) {
  Digest hash{};
  Identity reader{};
  const auto prefix = prepareUnboundBackupReview(hash, reader);
  ASSERT_FALSE(prefix.empty());
  auto& hal = inventory_hal_test::state;
  const auto baseline = hal;
  const auto destination = prefix + "-00";
  auto backups = makeUniqueNoThrow<HalCourseBaselineReviewBackup>(scratch, [](void*) { return true; }, nullptr);
  ASSERT_TRUE(backups);
  for (unsigned fault = 0; fault < 5; ++fault) {
    hal = baseline;
    if (fault == 0) {
      hal.failWritePath = destination + ".tmp";
      hal.failMatchingWrite = 2;
    }
    if (fault == 1) hal.failSyncPath = destination + ".tmp";
    if (fault == 2) hal.failClosePath = destination + ".tmp";
    if (fault == 3) hal.failRename = hal.renames + 1;
    if (fault == 4) hal.failRenameAfter = hal.renames + 1;
    EXPECT_FALSE(backups->preserveUnbound(hash, reader, generation, declaration.manifest.logicalIdentity));
    EXPECT_FALSE(backups->unboundComplete());
    for (const auto& [path, data] : baseline.files) EXPECT_EQ(hal.files.at(path), data);
    hal.failWritePath.clear();
    hal.failSyncPath.clear();
    hal.failClosePath.clear();
    hal.failRename = hal.failRenameAfter = 0;
    ASSERT_TRUE(backups->preserveUnbound(hash, reader, generation, declaration.manifest.logicalIdentity));
    EXPECT_EQ(hal.files.at(destination), baseline.files.at("/tinta/ITEMS.BIN"));
    EXPECT_FALSE(hal.files.contains(destination + ".tmp"));
  }
  hal.files[destination][0] ^= 1;
  const auto conflicting = hal.files;
  EXPECT_FALSE(backups->preserveUnbound(hash, reader, generation, declaration.manifest.logicalIdentity));
  EXPECT_FALSE(backups->verifyStoredUnbound(hash, reader, generation, declaration.manifest.logicalIdentity));
  EXPECT_EQ(hal.files, conflicting);
}

TEST_F(HalCourseTransferTest, NativeUnboundReviewReturnsPagesOnlyAfterBackupRecovery) {
  Digest hash{};
  Identity reader{};
  const auto prefix = prepareUnboundBackupReview(hash, reader);
  ASSERT_FALSE(prefix.empty());
  auto& hal = inventory_hal_test::state;
  const auto originals = hal.files;
  auto store = makeUniqueNoThrow<HalCourseBaselineReviewStore>(
      std::span(scratch).subspan(COURSE_BASELINE_REVIEW_MAX_SIZE), [](void*) { return true; }, nullptr);
  ASSERT_TRUE(store);
  CourseBaselineReviewPageRequest request;
  request.generation = generation;
  request.course = declaration.manifest.logicalIdentity;
  auto response = std::span(scratch).last(MAX_CONTROL_PAYLOAD);
  hal.failWritePath = prefix + "-00.tmp";
  hal.failMatchingWrite = 1;
  hal.matchingWrites = 0;
  EXPECT_EQ(handleHalCourseBaselineReviewRequest(
                *store, reader, generation, request, [](void*) { return true; }, nullptr, scratch, response),
            0u);
  for (const auto& [path, data] : originals) EXPECT_EQ(hal.files.at(path), data);
  hal.failWritePath.clear();
  const auto size = handleHalCourseBaselineReviewRequest(
      *store, reader, generation, request, [](void*) { return true; }, nullptr, scratch, response);
  ASSERT_GT(size, 0u);
  CourseBaselineReviewPageView page;
  ASSERT_TRUE(decodeCourseBaselineReviewPage(response.first(size), page));
  EXPECT_EQ(page.hash, hash);
  EXPECT_EQ(hal.files.at(prefix + "-00"), originals.at("/tinta/ITEMS.BIN"));
  EXPECT_EQ(hal.files.at(prefix + "-01"), originals.at("/tinta/usage.bin"));
  for (const auto& [path, data] : originals) EXPECT_EQ(hal.files.at(path), data);
}

TEST_F(HalCourseTransferTest, ReviewedBaselineBackupsRecoverWriteSyncCloseAndRenameBoundaries) {
  std::vector<uint8_t> encoded;
  Digest hash{};
  Identity reader{};
  prepareBaselineReview(encoded, hash, reader);
  ASSERT_FALSE(HasFatalFailure());
  auto& hal = inventory_hal_test::state;
  std::array<char, 112> sourcePath{};
  ASSERT_TRUE(courseStatePath(declaration.manifest.logicalIdentity, "items.bin", sourcePath));
  hal.files[sourcePath.data()] = std::vector<uint8_t>(12000, 17);
  auto capture = makeUniqueNoThrow<HalCourseBaselineReviewCapture>(scratch, [](void*) { return true; }, nullptr);
  ASSERT_TRUE(capture);
  ASSERT_EQ(capture->capture(reader, generation, declaration.manifest.logicalIdentity), CourseBaselineReviewResult::Ok);
  hash = *capture->hash();
  encoded.assign(capture->bytes().begin(), capture->bytes().end());
  capture.reset();
  auto store = makeUniqueNoThrow<HalCourseBaselineReviewStore>(
      std::span(scratch).subspan(COURSE_BASELINE_REVIEW_MAX_SIZE), [](void*) { return true; }, nullptr);
  ASSERT_TRUE(store);
  ASSERT_EQ(store->publish(encoded, hash), CourseBaselineReviewStoreResult::Ok);
  std::string path = store->path();
  path.replace(path.find("course-review-"), strlen("course-review-"), "course-review-state-");
  path += "-00";
  store.reset();
  const auto baseline = hal;
  auto backups = makeUniqueNoThrow<HalCourseBaselineReviewBackup>(scratch, [](void*) { return true; }, nullptr);
  ASSERT_TRUE(backups);
  for (unsigned fault = 0; fault < 5; ++fault) {
    SCOPED_TRACE(fault);
    hal = baseline;
    if (fault == 0) {
      hal.failWritePath = path + ".tmp";
      hal.failMatchingWrite = 2;
      hal.matchingWrites = 0;
    }
    if (fault == 1) hal.failSyncPath = path + ".tmp";
    if (fault == 2) hal.failClosePath = path + ".tmp";
    if (fault == 3) hal.failRename = hal.renames + 1;
    if (fault == 4) hal.failRenameAfter = hal.renames + 1;
    EXPECT_FALSE(backups->preserve(hash, reader, generation, declaration.manifest.logicalIdentity));
    EXPECT_FALSE(backups->complete());
    for (const auto& [name, data] : baseline.files) EXPECT_EQ(hal.files.at(name), data);
    hal.failWritePath.clear();
    hal.failSyncPath.clear();
    hal.failClosePath.clear();
    hal.failRename = hal.failRenameAfter = 0;
    ASSERT_TRUE(backups->preserve(hash, reader, generation, declaration.manifest.logicalIdentity));
    EXPECT_EQ(hal.files.at(path), hal.files.at(sourcePath.data()));
    EXPECT_FALSE(hal.files.contains(path + ".tmp"));
  }
}

TEST_F(HalCourseTransferTest, ReviewedBaselineBackupsRefuseChangedStateAndForeignEvidence) {
  std::vector<uint8_t> encoded;
  Digest hash{};
  Identity reader{};
  prepareBaselineReview(encoded, hash, reader);
  ASSERT_FALSE(HasFatalFailure());
  auto& hal = inventory_hal_test::state;
  std::array<char, 112> sourcePath{};
  ASSERT_TRUE(courseStatePath(declaration.manifest.logicalIdentity, "items.bin", sourcePath));
  auto store = makeUniqueNoThrow<HalCourseBaselineReviewStore>(
      std::span(scratch).subspan(COURSE_BASELINE_REVIEW_MAX_SIZE), [](void*) { return true; }, nullptr);
  ASSERT_TRUE(store);
  ASSERT_EQ(store->publish(encoded, hash), CourseBaselineReviewStoreResult::Ok);
  std::string path = store->path();
  path.replace(path.find("course-review-"), strlen("course-review-"), "course-review-state-");
  path += "-00";
  store.reset();
  const auto baseline = hal;
  auto backups = makeUniqueNoThrow<HalCourseBaselineReviewBackup>(scratch, [](void*) { return true; }, nullptr);
  ASSERT_TRUE(backups);
  for (unsigned fault = 0; fault < 7; ++fault) {
    SCOPED_TRACE(fault);
    hal = baseline;
    if (fault == 0) hal.files[sourcePath.data()][0] ^= 1;
    if (fault == 1) {
      auto added = std::string(sourcePath.data());
      added.replace(added.find("items.bin"), 9, "new.bin");
      hal.files[added] = {1};
    }
    if (fault == 2) hal.files[path + ".tmp"] = {18};
    if (fault == 3) hal.files[path] = {18};
    if (fault == 4) {
      hal.files[path] = {17};
      hal.files[path + ".tmp"] = {17};
    }
    if (fault == 5) {
      hal.files[path] = {17};
      auto duplicate = path;
      duplicate[std::string(TRANSFER_DIRECTORY).size() + 1] = 'C';
      hal.files[duplicate] = {17};
    }
    if (fault == 6) hal.readErrorPath = sourcePath.data();
    const auto before = hal.files;
    EXPECT_FALSE(backups->preserve(hash, reader, generation, declaration.manifest.logicalIdentity));
    EXPECT_FALSE(backups->complete());
    EXPECT_EQ(hal.files, before);
  }
  hal = baseline;
  auto wrongGeneration = generation;
  wrongGeneration[0] ^= 1;
  EXPECT_FALSE(backups->preserve(hash, reader, wrongGeneration, declaration.manifest.logicalIdentity));
  EXPECT_EQ(hal.files, baseline.files);
  backups.reset();
  struct UnexpectedWriter {
    const char* source;
    const char* published;
    bool changed = false;
  } writer{sourcePath.data(), path.c_str()};
  backups = makeUniqueNoThrow<HalCourseBaselineReviewBackup>(
      scratch,
      [](void* context) {
        auto& writer = *static_cast<UnexpectedWriter*>(context);
        auto& files = inventory_hal_test::state.files;
        if (!writer.changed && files.contains(writer.published)) {
          files.at(writer.source)[0] ^= 1;
          writer.changed = true;
        }
        return true;
      },
      &writer);
  ASSERT_TRUE(backups);
  EXPECT_FALSE(backups->preserve(hash, reader, generation, declaration.manifest.logicalIdentity));
  EXPECT_TRUE(writer.changed);
  EXPECT_FALSE(backups->complete());
  EXPECT_EQ(hal.files.at(path), baseline.files.at(sourcePath.data()));
  EXPECT_NE(hal.files.at(sourcePath.data()), baseline.files.at(sourcePath.data()));
  hal.files[sourcePath.data()] = baseline.files.at(sourcePath.data());
  ASSERT_TRUE(backups->preserve(hash, reader, generation, declaration.manifest.logicalIdentity));
}

TEST_F(HalCourseTransferTest, ReviewedBaselineBackupsRetainSourceSpellingAndPresentJournalFiles) {
  std::vector<uint8_t> encoded;
  Digest hash{};
  Identity reader{};
  prepareBaselineReview(encoded, hash, reader);
  ASSERT_FALSE(HasFatalFailure());
  auto& hal = inventory_hal_test::state;
  std::array<char, 112> sourcePath{};
  ASSERT_TRUE(courseStatePath(declaration.manifest.logicalIdentity, "items.bin", sourcePath));
  std::string uppercase = sourcePath.data();
  uppercase.replace(uppercase.find("items.bin"), 9, "ITEMS.BIN");
  hal.files[uppercase] = hal.files.at(sourcePath.data());
  hal.files.erase(sourcePath.data());
  hal.directories[TINTA_JOURNAL_DIRECTORY] = {};
  hal.files[TINTA_JOURNAL_EVENTS] = {11, 12, 13};
  hal.files[TINTA_JOURNAL_HEADER_B] = {21, 22};
  auto capture = makeUniqueNoThrow<HalCourseBaselineReviewCapture>(scratch, [](void*) { return true; }, nullptr);
  ASSERT_TRUE(capture);
  ASSERT_EQ(capture->capture(reader, generation, declaration.manifest.logicalIdentity), CourseBaselineReviewResult::Ok);
  hash = *capture->hash();
  encoded.assign(capture->bytes().begin(), capture->bytes().end());
  capture.reset();
  auto store = makeUniqueNoThrow<HalCourseBaselineReviewStore>(
      std::span(scratch).subspan(COURSE_BASELINE_REVIEW_MAX_SIZE), [](void*) { return true; }, nullptr);
  ASSERT_TRUE(store);
  ASSERT_EQ(store->publish(encoded, hash), CourseBaselineReviewStoreResult::Ok);
  std::string prefix = store->path();
  prefix.replace(prefix.find("course-review-"), strlen("course-review-"), "course-review-state-");
  store.reset();
  const auto baseline = hal;
  auto backups = makeUniqueNoThrow<HalCourseBaselineReviewBackup>(scratch, [](void*) { return true; }, nullptr);
  ASSERT_TRUE(backups);
  ASSERT_TRUE(backups->preserve(hash, reader, generation, declaration.manifest.logicalIdentity));
  EXPECT_EQ(hal.files.at(prefix + "-00"), hal.files.at(uppercase));
  EXPECT_EQ(hal.files.at(prefix + "-01"), hal.files.at(TINTA_JOURNAL_EVENTS));
  EXPECT_FALSE(hal.files.contains(prefix + "-02"));
  EXPECT_EQ(hal.files.at(prefix + "-03"), hal.files.at(TINTA_JOURNAL_HEADER_B));
  for (const auto& [path, data] : baseline.files) EXPECT_EQ(hal.files.at(path), data);
  const auto complete = hal;
  hal.files[TINTA_JOURNAL_EVENTS][1] ^= 1;
  const auto changedFiles = hal.files;
  EXPECT_FALSE(backups->preserve(hash, reader, generation, declaration.manifest.logicalIdentity));
  EXPECT_EQ(hal.files, changedFiles);
  hal = complete;
  ASSERT_TRUE(backups->preserve(hash, reader, generation, declaration.manifest.logicalIdentity));
  hal.files.erase(TINTA_JOURNAL_HEADER_B);
  const auto missingHeaderFiles = hal.files;
  EXPECT_FALSE(backups->preserve(hash, reader, generation, declaration.manifest.logicalIdentity));
  EXPECT_EQ(hal.files, missingHeaderFiles);
}

TEST_F(HalCourseTransferTest, BaselinePreparationRequiresMissingHistoryAndExactAuthenticatedRequest) {
  std::vector<uint8_t> encoded;
  Digest hash{};
  Identity reader{};
  prepareBaselineReview(encoded, hash, reader);
  ASSERT_FALSE(HasFatalFailure());
  auto& hal = inventory_hal_test::state;
  auto store = makeUniqueNoThrow<HalCourseBaselineReviewStore>(
      std::span(scratch).subspan(COURSE_BASELINE_REVIEW_MAX_SIZE), [](void*) { return true; }, nullptr);
  ASSERT_TRUE(store);
  ASSERT_EQ(store->publish(encoded, hash), CourseBaselineReviewStoreResult::Ok);
  store.reset();
  CourseBaselineImportRequest request{generation, declaration.state.owner, declaration.state.transaction,
                                      declaration.manifest, hash};
  bool permitted = true;
  auto preparation = makeUniqueNoThrow<HalCourseBaselineImportPreparation>(
      reader, generation, declaration.state.owner, scratch, [](void* context) { return *static_cast<bool*>(context); },
      &permitted);
  ASSERT_TRUE(preparation);
  const auto retainedHistoryFiles = hal.files;
  EXPECT_FALSE(preparation->prepare(request, declaration));
  EXPECT_EQ(preparation->prepared(), nullptr);
  EXPECT_EQ(hal.files, retainedHistoryFiles);
  for (auto iterator = hal.files.begin(); iterator != hal.files.end();) {
    if (iterator->first.starts_with("/.crosspoint/companion/removal-done-"))
      iterator = hal.files.erase(iterator);
    else
      ++iterator;
  }
  const auto missingHistoryFiles = hal.files;
  ASSERT_TRUE(preparation->prepare(request, declaration));
  ASSERT_NE(preparation->prepared(), nullptr);
  EXPECT_EQ(*preparation->prepared(), request);
  for (const auto& [path, data] : missingHistoryFiles) EXPECT_EQ(hal.files.at(path), data);
  const auto preserved = hal.files;
  ASSERT_TRUE(preparation->prepare(request, declaration));
  EXPECT_EQ(hal.files, preserved);
  for (unsigned field = 0; field < 5; ++field) {
    auto wrong = request;
    if (field == 0) wrong.owner[0] ^= 1;
    if (field == 1) wrong.generation[0] ^= 1;
    if (field == 2) wrong.transaction[0] ^= 1;
    if (field == 3) wrong.manifest.contentHash[0] ^= 1;
    if (field == 4) wrong.reviewHash[0] ^= 1;
    EXPECT_FALSE(preparation->prepare(wrong, declaration));
    EXPECT_EQ(preparation->prepared(), nullptr);
    EXPECT_EQ(hal.files, preserved);
  }
  ASSERT_TRUE(preparation->prepare(request, declaration));
  permitted = false;
  EXPECT_EQ(preparation->prepared(), nullptr);
  permitted = true;
  EXPECT_EQ(preparation->prepared(), nullptr);
  ASSERT_TRUE(preparation->prepare(request, declaration));
  preparation->close();
  EXPECT_EQ(preparation->prepared(), nullptr);
  preparation.reset();
  auto receipt = retainedHistoryFiles.begin();
  while (receipt != retainedHistoryFiles.end() && !receipt->first.starts_with("/.crosspoint/companion/removal-done-"))
    ++receipt;
  ASSERT_NE(receipt, retainedHistoryFiles.end());
  const auto copy = std::find_if(preserved.begin(), preserved.end(), [](const auto& entry) {
    return entry.first.starts_with("/.crosspoint/companion/course-review-state-") && entry.first.ends_with("-00");
  });
  ASSERT_NE(copy, preserved.end());
  struct NewReceipt {
    const char* copyPath;
    const char* receiptPath;
    const std::vector<uint8_t>* bytes;
    bool inserted = false;
  } writer{copy->first.c_str(), receipt->first.c_str(), &receipt->second};
  hal.files = missingHistoryFiles;
  preparation = makeUniqueNoThrow<HalCourseBaselineImportPreparation>(
      reader, generation, declaration.state.owner, scratch,
      [](void* context) {
        auto& writer = *static_cast<NewReceipt*>(context);
        auto& files = inventory_hal_test::state.files;
        if (!writer.inserted && files.contains(writer.copyPath)) {
          files[writer.receiptPath] = *writer.bytes;
          writer.inserted = true;
        }
        return true;
      },
      &writer);
  ASSERT_TRUE(preparation);
  EXPECT_FALSE(preparation->prepare(request, declaration));
  EXPECT_TRUE(writer.inserted);
  EXPECT_EQ(preparation->prepared(), nullptr);
  EXPECT_EQ(hal.files.at(writer.copyPath), copy->second);
  EXPECT_EQ(hal.files.at(writer.receiptPath), *writer.bytes);
  for (const auto& [path, data] : missingHistoryFiles) EXPECT_EQ(hal.files.at(path), data);
}

TEST_F(HalCourseTransferTest, BaselinePreparationRefusesRetainedArchivesAndUnavailableHistoryOrHeap) {
  std::vector<uint8_t> encoded;
  Digest hash{};
  Identity reader{};
  prepareBaselineReview(encoded, hash, reader);
  ASSERT_FALSE(HasFatalFailure());
  auto& hal = inventory_hal_test::state;
  for (auto iterator = hal.files.begin(); iterator != hal.files.end();) {
    if (iterator->first.starts_with("/.crosspoint/companion/removal-done-"))
      iterator = hal.files.erase(iterator);
    else
      ++iterator;
  }
  auto store = makeUniqueNoThrow<HalCourseBaselineReviewStore>(
      std::span(scratch).subspan(COURSE_BASELINE_REVIEW_MAX_SIZE), [](void*) { return true; }, nullptr);
  ASSERT_TRUE(store);
  ASSERT_EQ(store->publish(encoded, hash), CourseBaselineReviewStoreResult::Ok);
  const std::string sealedPath = store->path();
  store.reset();
  CourseBaselineImportRequest request{generation, declaration.state.owner, declaration.state.transaction,
                                      declaration.manifest, hash};
  auto preparation = makeUniqueNoThrow<HalCourseBaselineImportPreparation>(
      reader, generation, declaration.state.owner, scratch, [](void*) { return true; }, nullptr);
  ASSERT_TRUE(preparation);
  const auto baseline = hal;
  for (unsigned fault = 0; fault < 6; ++fault) {
    SCOPED_TRACE(fault);
    hal = baseline;
    if (fault == 0) hal.directoryErrorPath = TRANSFER_DIRECTORY;
    if (fault == 1) hal.files["/.crosspoint/companion/removal-done-bad"] = {1};
    if (fault == 2) hal.files.erase(sealedPath);
    if (fault == 3) {
      companion_memory_test::internal.freeBytes = 50 * 1024 + 1;
    }
    if (fault == 4) {
      companion_memory_test::internal.largestBlockBytes = 1;
    }
    if (fault == 5) {
      hal.files["/tinta/baseline-input.pack"] = bytes;
      auto archive = makeUniqueNoThrow<HalCoursePackArchive>(scratch, [](void*) { return true; }, nullptr);
      ASSERT_TRUE(archive);
      ASSERT_EQ(archive->publish(declaration.manifest, "/tinta/baseline-input.pack"), CourseArchiveResult::Ok);
    }
    const auto before = hal.files;
    EXPECT_FALSE(preparation->prepare(request, declaration));
    EXPECT_EQ(preparation->prepared(), nullptr);
    EXPECT_EQ(hal.files, before);
    companion_memory_test::internal = {1024 * 1024, 1024 * 1024, 1024 * 1024, 1024 * 1024};
  }
  hal = baseline;
  ASSERT_TRUE(preparation->prepare(request, declaration));
}

TEST_F(HalCourseTransferTest, NativeBaselineConsentPersistsOnlyPreparedRequestsAndLoadsWithoutAnApprovalLoan) {
  CourseBaselineImportRequest request;
  Identity reader{};
  std::string path;
  prepareBaselineApproval(request, reader, path);
  ASSERT_FALSE(HasFatalFailure());
  auto& hal = inventory_hal_test::state;
  const auto originals = hal.files;
  bool permitted = false;
  auto consent = makeUniqueNoThrow<HalCourseBaselineImportConsentStore>(
      reader, generation, declaration.state.owner, scratch, [](void* context) { return *static_cast<bool*>(context); },
      &permitted);
  ASSERT_TRUE(consent);
  EXPECT_EQ(consent->approve(request, declaration), CourseBaselineConsentResult::Busy);
  EXPECT_EQ(consent->approved(), nullptr);
  EXPECT_EQ(hal.files, originals);
  permitted = true;
  ASSERT_EQ(consent->approve(request, declaration), CourseBaselineConsentResult::Ok);
  ASSERT_NE(consent->approved(), nullptr);
  EXPECT_EQ(*consent->approved(), request);
  ASSERT_TRUE(hal.files.contains(path));
  CourseBaselineImportRequest decoded;
  ASSERT_TRUE(decodeCourseBaselineImportRequest(hal.files.at(path), decoded));
  EXPECT_EQ(decoded, request);
  for (const auto& [name, data] : originals) EXPECT_EQ(hal.files.at(name), data);
  const auto complete = hal.files;
  const auto renames = hal.renames;
  ASSERT_EQ(consent->approve(request, declaration), CourseBaselineConsentResult::Ok);
  EXPECT_EQ(hal.files, complete);
  EXPECT_EQ(hal.renames, renames);
  CourseBaselineImportRequest loaded;
  ASSERT_EQ(consent->load(request.transaction, loaded), CourseBaselineConsentResult::Ok);
  EXPECT_EQ(loaded, request);
  EXPECT_EQ(consent->approved(), nullptr);
  ASSERT_EQ(consent->approve(request, declaration), CourseBaselineConsentResult::Ok);
  permitted = false;
  EXPECT_EQ(consent->approved(), nullptr);
  permitted = true;
  EXPECT_EQ(consent->approved(), nullptr);
  consent.reset();
  for (unsigned identity = 0; identity < 3; ++identity) {
    auto expectedReader = reader, expectedGeneration = generation, expectedOwner = declaration.state.owner;
    if (identity == 0) expectedReader[0] ^= 1;
    if (identity == 1) expectedGeneration[0] ^= 1;
    if (identity == 2) expectedOwner[0] ^= 1;
    consent = makeUniqueNoThrow<HalCourseBaselineImportConsentStore>(
        expectedReader, expectedGeneration, expectedOwner, scratch, [](void*) { return true; }, nullptr);
    ASSERT_TRUE(consent);
    loaded = {};
    const auto previous = loaded;
    EXPECT_NE(consent->load(request.transaction, loaded), CourseBaselineConsentResult::Ok);
    EXPECT_EQ(loaded, previous);
    EXPECT_EQ(hal.files, complete);
    EXPECT_EQ(consent->approved(), nullptr);
    consent.reset();
  }
}

TEST_F(HalCourseTransferTest, NativeBaselineConsentRecoversAcknowledgementsAndPreservesTornOrConflictingEvidence) {
  CourseBaselineImportRequest request;
  Identity reader{};
  std::string path;
  prepareBaselineApproval(request, reader, path);
  ASSERT_FALSE(HasFatalFailure());
  auto& hal = inventory_hal_test::state;
  auto consent = makeUniqueNoThrow<HalCourseBaselineImportConsentStore>(
      reader, generation, declaration.state.owner, scratch, [](void*) { return true; }, nullptr);
  ASSERT_TRUE(consent);
  // Obtain the rename count after preparation, before the consent publication.
  auto preparation = makeUniqueNoThrow<HalCourseBaselineImportPreparation>(
      reader, generation, declaration.state.owner, scratch, [](void*) { return true; }, nullptr);
  ASSERT_TRUE(preparation);
  ASSERT_TRUE(preparation->prepare(request, declaration));
  preparation.reset();
  const auto baseline = hal;
  for (unsigned fault = 0; fault < 6; ++fault) {
    SCOPED_TRACE(fault);
    hal = baseline;
    if (fault == 0) hal.failSyncPath = path + ".tmp";
    if (fault == 1) hal.failClosePath = path + ".tmp";
    if (fault == 2) hal.failRename = hal.renames + 1;
    if (fault == 3) hal.failRenameAfter = hal.renames + 1;
    if (fault == 4) {
      hal.failWritePath = path + ".tmp";
      hal.failMatchingWrite = 1;
      hal.matchingWrites = 0;
    }
    if (fault == 5) hal.corruptWritePath = path + ".tmp";
    EXPECT_NE(consent->approve(request, declaration), CourseBaselineConsentResult::Ok);
    EXPECT_EQ(consent->approved(), nullptr);
    for (const auto& [name, data] : baseline.files) EXPECT_EQ(hal.files.at(name), data);
    hal.failSyncPath.clear();
    hal.failClosePath.clear();
    hal.failWritePath.clear();
    hal.corruptWritePath.clear();
    hal.failRename = hal.failRenameAfter = 0;
    if (fault < 5) {
      ASSERT_EQ(consent->approve(request, declaration), CourseBaselineConsentResult::Ok);
      CourseBaselineImportRequest loaded;
      ASSERT_EQ(consent->load(request.transaction, loaded), CourseBaselineConsentResult::Ok);
      EXPECT_EQ(loaded, request);
    } else {
      const auto torn = hal.files;
      EXPECT_EQ(consent->approve(request, declaration), CourseBaselineConsentResult::Corrupt);
      EXPECT_EQ(hal.files, torn);
    }
  }
  hal = baseline;
  ASSERT_EQ(consent->approve(request, declaration), CourseBaselineConsentResult::Ok);
  const auto approved = hal;
  for (unsigned conflict = 0; conflict < 3; ++conflict) {
    hal = approved;
    if (conflict == 0) {
      auto foreign = request;
      foreign.manifest.contentHash[0] ^= 1;
      ASSERT_TRUE(encodeCourseBaselineImportRequest(foreign, hal.files.at(path)));
    }
    if (conflict == 1) hal.files[path + ".tmp"] = hal.files.at(path);
    if (conflict == 2) {
      auto duplicate = path;
      duplicate[std::string(TRANSFER_DIRECTORY).size() + 1] = 'C';
      hal.files[duplicate] = hal.files.at(path);
    }
    const auto evidence = hal.files;
    EXPECT_NE(consent->approve(request, declaration), CourseBaselineConsentResult::Ok);
    EXPECT_EQ(consent->approved(), nullptr);
    EXPECT_EQ(hal.files, evidence);
  }
}

TEST_F(HalCourseTransferTest, NativeBaselineConsentTornRetryRequiresUnchangedReviewedLearnerState) {
  for (const bool changed : {false, true}) {
    SetUp();
    SCOPED_TRACE(changed);
    CourseBaselineImportRequest request;
    Identity reader{};
    std::string path;
    prepareBaselineApproval(request, reader, path);
    ASSERT_FALSE(HasFatalFailure());
    auto preparation = makeUniqueNoThrow<HalCourseBaselineImportPreparation>(
        reader, generation, request.owner, scratch, [](void*) { return true; }, nullptr);
    ASSERT_TRUE(preparation);
    ASSERT_TRUE(preparation->prepare(request, declaration));
    preparation.reset();
    std::array<uint8_t, COURSE_BASELINE_IMPORT_REQUEST_SIZE> encoded{};
    ASSERT_TRUE(encodeCourseBaselineImportRequest(request, encoded));
    auto& hal = inventory_hal_test::state;
    hal.files[path + ".tmp"] = {encoded.begin(), encoded.begin() + 100};
    if (changed) {
      std::array<char, COURSE_STATE_PATH_SIZE> items{};
      ASSERT_TRUE(courseStatePath(request.manifest.logicalIdentity, "items.bin", items));
      ASSERT_TRUE(hal.files.contains(items.data()));
      ASSERT_FALSE(hal.files.at(items.data()).empty());
      hal.files.at(items.data()).back() ^= 1;
    }
    const auto evidence = hal.files;
    auto consent = makeUniqueNoThrow<HalCourseBaselineImportConsentStore>(
        reader, generation, request.owner, scratch, [](void*) { return true; }, nullptr);
    ASSERT_TRUE(consent);
    CourseBaselineImportRequest loaded;
    EXPECT_EQ(consent->load(request.transaction, loaded), CourseBaselineConsentResult::Missing);
    EXPECT_EQ(hal.files, evidence);
    const auto result = consent->approve(request, declaration);
    if (changed) {
      EXPECT_NE(result, CourseBaselineConsentResult::Ok);
      EXPECT_EQ(consent->approved(), nullptr);
      EXPECT_EQ(hal.files, evidence);
    } else {
      ASSERT_EQ(result, CourseBaselineConsentResult::Ok);
      EXPECT_FALSE(hal.files.contains(path + ".tmp"));
      ASSERT_EQ(consent->load(request.transaction, loaded), CourseBaselineConsentResult::Ok);
      EXPECT_EQ(loaded, request);
      for (const auto& [name, data] : evidence) {
        if (name != path + ".tmp") {
          EXPECT_EQ(hal.files.at(name), data);
        }
      }
    }
  }
}

TEST_F(HalCourseTransferTest, NativeBaselineConsentRefusesApprovalWhenStateChangesDuringPersistence) {
  CourseBaselineImportRequest request;
  Identity reader{};
  std::string path;
  prepareBaselineApproval(request, reader, path);
  ASSERT_FALSE(HasFatalFailure());
  auto& hal = inventory_hal_test::state;
  std::array<char, 112> source{};
  ASSERT_TRUE(courseStatePath(declaration.manifest.logicalIdentity, "items.bin", source));
  const auto original = hal.files.at(source.data());
  struct UnexpectedWriter {
    const char* approval;
    const char* source;
    bool changed = false;
  } writer{path.c_str(), source.data()};
  auto consent = makeUniqueNoThrow<HalCourseBaselineImportConsentStore>(
      reader, generation, declaration.state.owner, scratch,
      [](void* context) {
        auto& writer = *static_cast<UnexpectedWriter*>(context);
        auto& files = inventory_hal_test::state.files;
        if (!writer.changed && files.contains(writer.approval)) {
          files.at(writer.source)[0] ^= 1;
          writer.changed = true;
        }
        return true;
      },
      &writer);
  ASSERT_TRUE(consent);
  EXPECT_NE(consent->approve(request, declaration), CourseBaselineConsentResult::Ok);
  EXPECT_TRUE(writer.changed);
  EXPECT_EQ(consent->approved(), nullptr);
  ASSERT_TRUE(hal.files.contains(path));
  const auto changedFiles = hal.files;
  CourseBaselineImportRequest loaded;
  ASSERT_EQ(consent->load(request.transaction, loaded), CourseBaselineConsentResult::Ok);
  EXPECT_EQ(loaded, request);
  EXPECT_EQ(consent->approved(), nullptr);
  EXPECT_NE(consent->approve(request, declaration), CourseBaselineConsentResult::Ok);
  EXPECT_EQ(hal.files, changedFiles);
  hal.files[source.data()] = original;
  ASSERT_EQ(consent->approve(request, declaration), CourseBaselineConsentResult::Ok);
  ASSERT_NE(consent->approved(), nullptr);
  EXPECT_EQ(*consent->approved(), request);
}

TEST_F(HalCourseTransferTest, BaselineUploadBeginsAfterNativeApprovalAndResumesWithoutChangingTheActivePack) {
  CourseBaselineImportRequest request;
  Identity reader{};
  std::string consentPath;
  prepareBaselineApproval(request, reader, consentPath);
  ASSERT_FALSE(HasFatalFailure());
  auto& hal = inventory_hal_test::state;
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  std::array<uint8_t, COURSE_BINDING_SIZE> activeBinding{};
  ASSERT_EQ(encodeCourseBinding(declaration.manifest, activeBinding), activeBinding.size());
  hal.files[COURSE_BINDING_PATH] = {activeBinding.begin(), activeBinding.end()};
  const auto originals = hal.files;
  auto consent = makeUniqueNoThrow<HalCourseBaselineImportConsentStore>(
      reader, generation, request.owner, scratch, [](void*) { return true; }, nullptr);
  ASSERT_TRUE(consent);
  auto authorize = [](void* context, const CourseBaselineImportRequest& request,
                      const TransferDeclaration& declaration) {
    return static_cast<HalCourseBaselineImportConsentStore*>(context)->approve(request, declaration) ==
           CourseBaselineConsentResult::Ok;
  };
  Transfer transfer(storage, scratch);
  ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
  ASSERT_EQ(beginCourseBaselineTransfer(transfer, request, generation, request.owner, authorize, consent.get()),
            TransferResult::Ok);
  ASSERT_NE(transfer.current(), nullptr);
  EXPECT_EQ(transfer.destination(), COURSE_BASELINE_DESTINATION);
  EXPECT_EQ(*transfer.contentManifest(), request.manifest);
  ASSERT_EQ(transfer.append(request.transaction, request.owner, 0, std::span(bytes).first(13)), TransferResult::Ok);
  const auto checkpoint = *transfer.current();
  Transfer restored(storage, scratch);
  ASSERT_EQ(restored.recover(generation), TransferResult::Ok);
  EXPECT_EQ(*restored.current(), checkpoint);
  const auto staged = hal.files.at(TRANSFER_STAGE);
  ASSERT_EQ(beginCourseBaselineTransfer(restored, request, generation, request.owner, authorize, consent.get()),
            TransferResult::Ok);
  EXPECT_EQ(restored.current()->durableOffset, 13);
  EXPECT_EQ(hal.files.at(TRANSFER_STAGE), staged);
  for (size_t offset = 13; offset < bytes.size();) {
    const auto count = std::min(size_t{1000}, bytes.size() - offset);
    ASSERT_EQ(restored.append(request.transaction, request.owner, offset, std::span(bytes).subspan(offset, count)),
              TransferResult::Ok);
    offset += count;
  }
  EXPECT_EQ(hal.files.at(TRANSFER_STAGE), bytes);
  // Installation remains unavailable until native archive/completion recovery is wired.
  EXPECT_EQ(restored.commit(request.transaction, request.owner), TransferResult::Invalid);
  EXPECT_EQ(restored.current()->phase, TransferPhase::Receiving);
  for (const auto& [path, data] : originals) EXPECT_EQ(hal.files.at(path), data);
  EXPECT_FALSE(hal.files.contains(COURSE_BASELINE_DESTINATION));
  ASSERT_EQ(restored.abort(request.transaction, request.owner), TransferResult::Ok);
  EXPECT_FALSE(hal.files.contains(TRANSFER_STAGE));
  EXPECT_TRUE(hal.files.contains(consentPath));
  for (const auto& [path, data] : originals) EXPECT_EQ(hal.files.at(path), data);
}

TEST_F(HalCourseTransferTest, BaselineUploadRejectsAuthorizationAndTransactionRetargetingBeforeMutation) {
  CourseBaselineImportRequest request;
  Identity reader{};
  std::string consentPath;
  prepareBaselineApproval(request, reader, consentPath);
  ASSERT_FALSE(HasFatalFailure());
  auto& hal = inventory_hal_test::state;
  Transfer transfer(storage, scratch);
  ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
  unsigned calls = 0;
  auto authorize = [](void* context, const CourseBaselineImportRequest&, const TransferDeclaration&) {
    ++*static_cast<unsigned*>(context);
    return true;
  };
  const auto originals = hal.files;
  std::array<uint8_t, 300> body{};
  ASSERT_EQ(encodeTransferDeclaration(declaration, body), TRANSFER_DECLARATION_SIZE);
  constexpr std::string_view destination = COURSE_BASELINE_DESTINATION;
  body[TRANSFER_DECLARATION_SIZE] = destination.size();
  std::copy(destination.begin(), destination.end(), body.begin() + TRANSFER_DECLARATION_SIZE + 1);
  std::array<uint8_t, 100> reply{};
  ASSERT_EQ(
      handleTransfer(transfer, Command::BeginTransfer,
                     std::span(body).first(TRANSFER_DECLARATION_SIZE + 1 + destination.size()), request.owner, reply),
      1);
  EXPECT_EQ(reply[0], static_cast<uint8_t>(TransferResult::Invalid));
  EXPECT_EQ(hal.files, originals);
  auto wrongOwner = request.owner, wrongGeneration = generation;
  wrongOwner[0] ^= 1;
  wrongGeneration[0] ^= 1;
  EXPECT_EQ(beginCourseBaselineTransfer(transfer, request, generation, wrongOwner, authorize, &calls),
            TransferResult::Unauthorized);
  EXPECT_EQ(beginCourseBaselineTransfer(transfer, request, wrongGeneration, request.owner, authorize, &calls),
            TransferResult::WrongStorage);
  EXPECT_EQ(beginCourseBaselineTransfer(transfer, request, generation, request.owner, nullptr, nullptr),
            TransferResult::Unauthorized);
  EXPECT_EQ(beginCourseBaselineTransfer(
                transfer, request, generation, request.owner,
                [](void*, const CourseBaselineImportRequest&, const TransferDeclaration&) { return false; }, nullptr),
            TransferResult::Unauthorized);
  EXPECT_EQ(calls, 0);
  EXPECT_EQ(hal.files, originals);
  ASSERT_EQ(beginCourseBaselineTransfer(transfer, request, generation, request.owner, authorize, &calls),
            TransferResult::Ok);
  const auto receiving = hal.files;
  auto retargeted = request;
  retargeted.manifest.contentHash[0] ^= 1;
  EXPECT_EQ(beginCourseBaselineTransfer(transfer, retargeted, generation, request.owner, authorize, &calls),
            TransferResult::Invalid);
  retargeted = request;
  retargeted.transaction[0] ^= 1;
  EXPECT_EQ(beginCourseBaselineTransfer(transfer, retargeted, generation, request.owner, authorize, &calls),
            TransferResult::Busy);
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(hal.files, receiving);
}

TEST_F(HalCourseTransferTest, BaselineUploadFreezesManifestBeforeApprovalReusesCallerBuffers) {
  CourseBaselineImportRequest request;
  Identity reader{};
  std::string consentPath;
  prepareBaselineApproval(request, reader, consentPath);
  ASSERT_FALSE(HasFatalFailure());
  const auto expected = request;
  struct Approval {
    HalCourseBaselineImportConsentStore* consent;
    CourseBaselineImportRequest* caller;
  } context;
  auto consent = makeUniqueNoThrow<HalCourseBaselineImportConsentStore>(
      reader, generation, request.owner, scratch, [](void*) { return true; }, nullptr);
  ASSERT_TRUE(consent);
  context = {consent.get(), &request};
  Transfer transfer(storage, scratch);
  ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
  ASSERT_EQ(beginCourseBaselineTransfer(
                transfer, request, generation, request.owner,
                [](void* context, const CourseBaselineImportRequest& request, const TransferDeclaration& declaration) {
                  auto& approval = *static_cast<Approval*>(context);
                  if (approval.consent->approve(request, declaration) != CourseBaselineConsentResult::Ok) return false;
                  *approval.caller = {};
                  return true;
                },
                &context),
            TransferResult::Ok);
  EXPECT_EQ(*transfer.contentManifest(), expected.manifest);
  EXPECT_EQ(transfer.current()->transaction, expected.transaction);
  EXPECT_EQ(transfer.current()->owner, expected.owner);
  EXPECT_EQ(transfer.current()->contentHash, expected.manifest.contentHash);
  EXPECT_EQ(transfer.destination(), COURSE_BASELINE_DESTINATION);
}

TEST_F(HalCourseTransferTest, ReviewedBaselineStoredCopiesRemainVerifiableAfterArchiveAndLearnerChanges) {
  CourseBaselineImportRequest request;
  Identity reader{};
  std::string consentPath;
  prepareBaselineApproval(request, reader, consentPath);
  ASSERT_FALSE(HasFatalFailure());
  auto& hal = inventory_hal_test::state;
  auto backups = makeUniqueNoThrow<HalCourseBaselineReviewBackup>(scratch, [](void*) { return true; }, nullptr);
  ASSERT_TRUE(backups);
  ASSERT_TRUE(backups->preserve(request.reviewHash, reader, generation, request.manifest.logicalIdentity));
  ASSERT_TRUE(backups->complete());
  ASSERT_TRUE(backups->verifyStored(request.reviewHash, reader, generation, request.manifest.logicalIdentity));
  EXPECT_FALSE(backups->complete());
  hal.files["/tinta/original-baseline.pack"] = bytes;
  auto archive = makeUniqueNoThrow<HalCoursePackArchive>(scratch, [](void*) { return true; }, nullptr);
  ASSERT_TRUE(archive);
  ASSERT_EQ(archive->publish(request.manifest, "/tinta/original-baseline.pack"), CourseArchiveResult::Ok);
  archive.reset();
  std::array<char, 112> source{};
  ASSERT_TRUE(courseStatePath(request.manifest.logicalIdentity, "items.bin", source));
  hal.files[source.data()][0] ^= 1;
  hal.directories[TINTA_JOURNAL_DIRECTORY] = {};
  hal.files[TINTA_JOURNAL_EVENTS] = {11, 12};
  hal.files[TINTA_JOURNAL_HEADER_B] = {21};
  const auto changedFiles = hal.files;
  EXPECT_FALSE(backups->preserve(request.reviewHash, reader, generation, request.manifest.logicalIdentity));
  ASSERT_TRUE(backups->verifyStored(request.reviewHash, reader, generation, request.manifest.logicalIdentity));
  EXPECT_FALSE(backups->complete());
  EXPECT_EQ(hal.files, changedFiles);
  hal.files.erase(source.data());
  const auto missingSourceFiles = hal.files;
  ASSERT_TRUE(backups->verifyStored(request.reviewHash, reader, generation, request.manifest.logicalIdentity));
  EXPECT_EQ(hal.files, missingSourceFiles);
}

TEST_F(HalCourseTransferTest, ReviewedBaselineStoredCopyVerificationRefusesIncompleteCorruptAndForeignEvidence) {
  CourseBaselineImportRequest request;
  Identity reader{};
  std::string consentPath;
  prepareBaselineApproval(request, reader, consentPath);
  ASSERT_FALSE(HasFatalFailure());
  auto& hal = inventory_hal_test::state;
  auto backups = makeUniqueNoThrow<HalCourseBaselineReviewBackup>(scratch, [](void*) { return true; }, nullptr);
  ASSERT_TRUE(backups);
  const auto original = hal.files;
  EXPECT_FALSE(backups->verifyStored(request.reviewHash, reader, generation, request.manifest.logicalIdentity));
  EXPECT_EQ(hal.files, original);
  ASSERT_TRUE(backups->preserve(request.reviewHash, reader, generation, request.manifest.logicalIdentity));
  std::string path;
  for (const auto& [name, data] : hal.files)
    if (name.starts_with("/.crosspoint/companion/course-review-state-") && name.ends_with("-00")) path = name;
  ASSERT_FALSE(path.empty());
  const auto baseline = hal;
  for (unsigned fault = 0; fault < 11; ++fault) {
    SCOPED_TRACE(fault);
    hal = baseline;
    auto expectedReader = reader, expectedGeneration = generation, expectedCourse = request.manifest.logicalIdentity;
    if (fault == 0) hal.files[path][0] ^= 1;
    if (fault == 1) hal.files.erase(path);
    if (fault == 2) hal.files[path + ".tmp"] = {17};
    if (fault == 3) {
      auto duplicate = path;
      duplicate[std::string(TRANSFER_DIRECTORY).size() + 1] = 'C';
      hal.files[duplicate] = {17};
    }
    if (fault == 4) hal.readErrorPath = path;
    if (fault == 5) hal.failSyncPath = path;
    if (fault == 6) hal.failClosePath = path;
    if (fault == 7) {
      auto absent = path;
      absent.back() = '1';
      hal.files[absent] = {};
    }
    if (fault == 8) expectedReader[0] ^= 1;
    if (fault == 9) expectedGeneration[0] ^= 1;
    if (fault == 10) expectedCourse[0] ^= 1;
    const auto before = hal.files;
    EXPECT_FALSE(backups->verifyStored(request.reviewHash, expectedReader, expectedGeneration, expectedCourse));
    EXPECT_FALSE(backups->complete());
    EXPECT_EQ(hal.files, before);
  }
  hal = baseline;
  ASSERT_TRUE(backups->verifyStored(request.reviewHash, reader, generation, request.manifest.logicalIdentity));
  EXPECT_FALSE(backups->complete());
}

namespace {
struct NativePublicationArtifacts {
  bool permitted = true, archive = false, fresh = true, verified = true;
  unsigned preparations = 0, publications = 0, verifications = 0;
  CourseBaselinePublicationHooks hooks() {
    return {this,
            [](void* context, const CourseBaselinePublicationRecord&, bool recovering) {
              auto& state = *static_cast<NativePublicationArtifacts*>(context);
              ++state.preparations;
              return state.fresh || (recovering && state.archive);
            },
            [](void* context, const CourseBaselinePublicationRecord&) {
              auto& state = *static_cast<NativePublicationArtifacts*>(context);
              ++state.publications;
              state.archive = true;
              return true;
            },
            [](void* context, const CourseBaselinePublicationRecord&) {
              auto& state = *static_cast<NativePublicationArtifacts*>(context);
              ++state.verifications;
              return state.archive && state.verified;
            }};
  }
  static bool allowed(void* context) { return static_cast<NativePublicationArtifacts*>(context)->permitted; }
};
}  // namespace

TEST_F(HalCourseTransferTest, NativeBaselinePublicationChecksContextAndReplaysWithoutFreshApproval) {
  CourseBaselinePublicationRecord record;
  std::string consentPath;
  prepareBaselineApproval(record.request, record.reader, consentPath);
  ASSERT_FALSE(HasFatalFailure());
  auto& hal = inventory_hal_test::state;
  const auto original = hal.files;
  NativePublicationArtifacts artifacts;
  auto store = makeUniqueNoThrow<HalCourseBaselinePublicationStore>(
      record.reader, generation, declaration.state.owner, scratch, NativePublicationArtifacts::allowed, &artifacts);
  ASSERT_TRUE(store);
  for (unsigned identity = 0; identity < 3; ++identity) {
    auto foreign = record;
    if (identity == 0) foreign.reader[0] ^= 0x80;
    if (identity == 1) foreign.request.generation[0] ^= 0x80;
    if (identity == 2) foreign.request.owner[0] ^= 0x80;
    EXPECT_EQ(store->publish(foreign, artifacts.hooks()), CourseBaselinePublicationResult::Conflict);
    EXPECT_EQ(hal.files, original);
    EXPECT_EQ(artifacts.preparations, 0u);
  }
  artifacts.permitted = false;
  EXPECT_EQ(store->publish(record, artifacts.hooks()), CourseBaselinePublicationResult::Busy);
  EXPECT_EQ(hal.files, original);
  artifacts.permitted = true;
  ASSERT_EQ(store->publish(record, artifacts.hooks()), CourseBaselinePublicationResult::Ok);
  ASSERT_NE(store->published(), nullptr);
  EXPECT_EQ(store->published()->phase, CourseBaselinePublicationPhase::Published);
  for (const auto& [path, data] : original) EXPECT_EQ(hal.files.at(path), data);
  const auto completed = hal.files;
  const auto renames = hal.renames;
  artifacts.fresh = false;
  store.reset();
  store = makeUniqueNoThrow<HalCourseBaselinePublicationStore>(
      record.reader, generation, declaration.state.owner, scratch, NativePublicationArtifacts::allowed, &artifacts);
  ASSERT_TRUE(store);
  ASSERT_EQ(store->publish(record, artifacts.hooks()), CourseBaselinePublicationResult::Ok);
  EXPECT_EQ(artifacts.preparations, 1u);
  EXPECT_EQ(artifacts.publications, 1u);
  EXPECT_EQ(hal.files, completed);
  EXPECT_EQ(hal.renames, renames);
  artifacts.permitted = false;
  EXPECT_EQ(store->published(), nullptr);
  artifacts.permitted = true;
  EXPECT_EQ(store->published(), nullptr);
  artifacts.verified = false;
  EXPECT_EQ(store->publish(record, artifacts.hooks()), CourseBaselinePublicationResult::VerificationFailed);
  EXPECT_EQ(store->published(), nullptr);
  EXPECT_EQ(hal.files, completed);
}

TEST_F(HalCourseTransferTest, NativeBaselinePublicationRecoversCheckedCloseAndRenameBoundaries) {
  CourseBaselinePublicationRecord record;
  std::string consentPath;
  prepareBaselineApproval(record.request, record.reader, consentPath);
  ASSERT_FALSE(HasFatalFailure());
  const auto prefix = consentPath.substr(0, consentPath.size() - std::string(".consent").size());
  auto& hal = inventory_hal_test::state;
  const auto initial = hal;
  for (unsigned phase = 0; phase < 2; ++phase) {
    for (unsigned fault = 0; fault < 6; ++fault) {
      SCOPED_TRACE(phase);
      SCOPED_TRACE(fault);
      hal = initial;
      const auto path = prefix + (phase == 0 ? ".prepared" : ".published");
      NativePublicationArtifacts artifacts;
      auto store = makeUniqueNoThrow<HalCourseBaselinePublicationStore>(
          record.reader, generation, declaration.state.owner, scratch, NativePublicationArtifacts::allowed, &artifacts);
      ASSERT_TRUE(store);
      if (fault == 0) hal.failSyncPath = path + ".tmp";
      if (fault == 1) hal.failClosePath = path + ".tmp";
      if (fault == 2) hal.failRename = hal.renames + phase + 1;
      if (fault == 3) hal.failRenameAfter = hal.renames + phase + 1;
      if (fault == 4) {
        hal.failWritePath = path + ".tmp";
        hal.failMatchingWrite = 1;
        hal.matchingWrites = 0;
      }
      if (fault == 5) hal.corruptWritePath = path + ".tmp";
      EXPECT_NE(store->publish(record, artifacts.hooks()), CourseBaselinePublicationResult::Ok);
      EXPECT_EQ(store->published(), nullptr);
      for (const auto& [name, data] : initial.files) EXPECT_EQ(hal.files.at(name), data);
      hal.failSyncPath.clear();
      hal.failClosePath.clear();
      hal.failWritePath.clear();
      hal.corruptWritePath.clear();
      hal.failRename = hal.failRenameAfter = 0;
      store.reset();
      store = makeUniqueNoThrow<HalCourseBaselinePublicationStore>(
          record.reader, generation, declaration.state.owner, scratch, NativePublicationArtifacts::allowed, &artifacts);
      ASSERT_TRUE(store);
      if (fault < 5) {
        if (phase == 1 && fault < 4) artifacts.fresh = false;
        ASSERT_EQ(store->publish(record, artifacts.hooks()), CourseBaselinePublicationResult::Ok);
        ASSERT_NE(store->published(), nullptr);
        for (const auto& [name, data] : initial.files) EXPECT_EQ(hal.files.at(name), data);
      } else {
        const auto torn = hal.files;
        EXPECT_EQ(store->publish(record, artifacts.hooks()), CourseBaselinePublicationResult::Corrupt);
        EXPECT_EQ(hal.files, torn);
      }
    }
  }
}

TEST_F(HalCourseTransferTest, NativeBaselinePublicationRejectsDuplicateAndIncompleteNamespaceEvidence) {
  CourseBaselinePublicationRecord record;
  std::string consentPath;
  prepareBaselineApproval(record.request, record.reader, consentPath);
  ASSERT_FALSE(HasFatalFailure());
  auto& hal = inventory_hal_test::state;
  NativePublicationArtifacts artifacts;
  auto store = makeUniqueNoThrow<HalCourseBaselinePublicationStore>(
      record.reader, generation, declaration.state.owner, scratch, NativePublicationArtifacts::allowed, &artifacts);
  ASSERT_TRUE(store);
  ASSERT_EQ(store->publish(record, artifacts.hooks()), CourseBaselinePublicationResult::Ok);
  const auto completed = hal;
  const auto path = consentPath.substr(0, consentPath.size() - 8) + ".prepared";
  for (unsigned fault = 0; fault < 3; ++fault) {
    hal = completed;
    if (fault == 0) {
      auto duplicate = path;
      duplicate[std::string(TRANSFER_DIRECTORY).size() + 1] = 'C';
      hal.files[duplicate] = hal.files.at(path);
    }
    if (fault == 1) hal.files[path + ".tmp"] = hal.files.at(path);
    if (fault == 2) hal.statErrorPath = path;
    const auto evidence = hal.files;
    const auto calls = artifacts.verifications;
    EXPECT_NE(store->publish(record, artifacts.hooks()), CourseBaselinePublicationResult::Ok);
    EXPECT_EQ(store->published(), nullptr);
    EXPECT_EQ(artifacts.verifications, calls);
    EXPECT_EQ(hal.files, evidence);
  }
}

TEST_F(HalCourseTransferTest, PublishedBaselineVerifierChecksActualArtifactsAndPreservesNewerLearnerState) {
  CourseBaselinePublicationRecord record;
  std::string consentPath;
  prepareBaselineApproval(record.request, record.reader, consentPath);
  ASSERT_FALSE(HasFatalFailure());
  auto& hal = inventory_hal_test::state;
  auto permission = [](void* context) { return *static_cast<bool*>(context); };
  bool permitted = true;
  auto consent = makeUniqueNoThrow<HalCourseBaselineImportConsentStore>(record.reader, generation, record.request.owner,
                                                                        scratch, permission, &permitted);
  ASSERT_TRUE(consent);
  ASSERT_EQ(consent->approve(record.request, declaration), CourseBaselineConsentResult::Ok);
  consent.reset();
  hal.files["/tinta/original.pack"] = bytes;
  auto archive = makeUniqueNoThrow<HalCoursePackArchive>(scratch, permission, &permitted);
  ASSERT_TRUE(archive);
  ASSERT_EQ(archive->publish(record.request.manifest, "/tinta/original.pack"), CourseArchiveResult::Ok);
  ASSERT_NE(archive->path(), nullptr);
  const auto archivePath = std::string(archive->path());
  archive.reset();
  std::array<char, COURSE_STATE_PATH_SIZE> items{};
  ASSERT_TRUE(courseStatePath(record.request.manifest.logicalIdentity, "items.bin", items));
  hal.files[items.data()] = {31, 37};
  hal.files.erase("/tinta/original.pack");
  const auto completed = hal;
  auto parser = makeUniqueNoThrow<tinta::core::pack::Pack>();
  ASSERT_TRUE(parser);
  auto verifier = makeUniqueNoThrow<HalCourseBaselineArchiveSession>(record.reader, generation, record.request.owner,
                                                                     scratch, *parser, permission, &permitted);
  ASSERT_TRUE(verifier);
  ASSERT_TRUE(verifier->verify(record));
  record.phase = CourseBaselinePublicationPhase::Published;
  ASSERT_TRUE(HalCourseBaselineArchiveSession::verifyPublication(verifier.get(), record));
  EXPECT_EQ(hal.files, completed.files);
  EXPECT_EQ(hal.renames, completed.renames);
  for (unsigned fault = 0; fault < 9; ++fault) {
    SCOPED_TRACE(fault);
    hal = completed;
    auto input = record;
    if (fault == 0) hal.files.erase(consentPath);
    if (fault == 1) hal.files[consentPath].back() ^= 1;
    if (fault == 2) hal.files[archivePath][0] ^= 1;
    if (fault == 3) hal.failSyncPath = archivePath;
    if (fault == 4) hal.failClosePath = archivePath;
    if (fault == 5) {
      for (auto iterator = hal.files.begin(); iterator != hal.files.end(); ++iterator) {
        if (iterator->first.starts_with("/.crosspoint/companion/course-review-state-")) {
          hal.files.erase(iterator);
          break;
        }
      }
    }
    if (fault == 6) input.reader[0] ^= 0x80;
    if (fault == 7) input.request.owner[0] ^= 0x80;
    if (fault == 8) permitted = false;
    const auto evidence = hal.files;
    EXPECT_FALSE(verifier->verify(input));
    EXPECT_EQ(hal.files, evidence);
    permitted = true;
  }
  hal = completed;
  EXPECT_TRUE(verifier->verify(record));
  EXPECT_EQ(hal.files.at(items.data()), std::vector<uint8_t>({31, 37}));
}

TEST_F(HalCourseTransferTest, PublishedBaselineVerifierRejectsHashMatchedMalformedPack) {
  // The archive's SHA proves the bytes, while the parser must prove their format.
  bytes[0] ^= 1;
  hashBytes();
  CourseBaselinePublicationRecord record;
  std::string consentPath;
  prepareBaselineApproval(record.request, record.reader, consentPath);
  ASSERT_FALSE(HasFatalFailure());
  auto& hal = inventory_hal_test::state;
  auto permission = [](void*) { return true; };
  auto consent = makeUniqueNoThrow<HalCourseBaselineImportConsentStore>(record.reader, generation, record.request.owner,
                                                                        scratch, permission, nullptr);
  ASSERT_TRUE(consent);
  ASSERT_EQ(consent->approve(record.request, declaration), CourseBaselineConsentResult::Ok);
  consent.reset();
  hal.files["/tinta/original.pack"] = bytes;
  auto archive = makeUniqueNoThrow<HalCoursePackArchive>(scratch, permission, nullptr);
  ASSERT_TRUE(archive);
  ASSERT_EQ(archive->publish(record.request.manifest, "/tinta/original.pack"), CourseArchiveResult::Ok);
  archive.reset();
  const auto evidence = hal.files;
  auto parser = makeUniqueNoThrow<tinta::core::pack::Pack>();
  ASSERT_TRUE(parser);
  auto verifier = makeUniqueNoThrow<HalCourseBaselineArchiveSession>(record.reader, generation, record.request.owner,
                                                                     scratch, *parser, permission, nullptr);
  ASSERT_TRUE(verifier);
  EXPECT_FALSE(verifier->verify(record));
  EXPECT_EQ(hal.files, evidence);
}

TEST_F(HalCourseTransferTest, NativeBaselineArchivePublicationRequiresIntentAndCompletesPersistedWorkflow) {
  CourseBaselinePublicationRecord record;
  std::string consentPath;
  prepareBaselineApproval(record.request, record.reader, consentPath);
  ASSERT_FALSE(HasFatalFailure());
  auto& hal = inventory_hal_test::state;
  auto permission = [](void*) { return true; };
  auto consent = makeUniqueNoThrow<HalCourseBaselineImportConsentStore>(record.reader, generation, record.request.owner,
                                                                        scratch, permission, nullptr);
  ASSERT_TRUE(consent);
  ASSERT_EQ(consent->approve(record.request, declaration), CourseBaselineConsentResult::Ok);
  consent.reset();
  hal.files["/tinta/original.pack"] = bytes;
  const auto initial = hal.files;
  auto parser = makeUniqueNoThrow<tinta::core::pack::Pack>();
  ASSERT_TRUE(parser);
  auto session = makeUniqueNoThrow<HalCourseBaselineArchiveSession>(record.reader, generation, record.request.owner,
                                                                    scratch, *parser, permission, nullptr);
  ASSERT_TRUE(session);
  EXPECT_FALSE(session->publishArchive(record, "/tinta/original.pack"));
  EXPECT_EQ(hal.files, initial);
  CourseBaselinePublicationHooks hooks{session.get(),
                                       // Fresh native approval above runs with no intervening state writer.
                                       [](void*, const CourseBaselinePublicationRecord&, bool) { return true; },
                                       [](void* context, const CourseBaselinePublicationRecord& request) {
                                         return static_cast<HalCourseBaselineArchiveSession*>(context)->publishArchive(
                                             request, "/tinta/original.pack");
                                       },
                                       HalCourseBaselineArchiveSession::verifyPublication};
  auto publication = makeUniqueNoThrow<HalCourseBaselinePublicationStore>(
      record.reader, generation, record.request.owner, scratch, permission, nullptr);
  ASSERT_TRUE(publication);
  ASSERT_EQ(publication->publish(record, hooks), CourseBaselinePublicationResult::Ok);
  ASSERT_NE(publication->published(), nullptr);
  hal.files.erase("/tinta/original.pack");
  std::array<char, COURSE_STATE_PATH_SIZE> items{};
  ASSERT_TRUE(courseStatePath(record.request.manifest.logicalIdentity, "items.bin", items));
  hal.files[items.data()] = {43};
  const auto completed = hal.files;
  hooks.verifyPrepared = [](void*, const CourseBaselinePublicationRecord&, bool) { return false; };
  hooks.publishArchive = [](void*, const CourseBaselinePublicationRecord&) { return false; };
  ASSERT_EQ(publication->publish(record, hooks), CourseBaselinePublicationResult::Ok);
  EXPECT_EQ(hal.files, completed);
}

TEST_F(HalCourseTransferTest, NativeBaselineArchivePublicationRefusesUnverifiedSourcesAndIntentWithoutMutation) {
  CourseBaselinePublicationRecord record;
  std::string consentPath;
  prepareBaselineApproval(record.request, record.reader, consentPath);
  ASSERT_FALSE(HasFatalFailure());
  auto& hal = inventory_hal_test::state;
  auto permission = [](void*) { return true; };
  auto consent = makeUniqueNoThrow<HalCourseBaselineImportConsentStore>(record.reader, generation, record.request.owner,
                                                                        scratch, permission, nullptr);
  ASSERT_TRUE(consent);
  ASSERT_EQ(consent->approve(record.request, declaration), CourseBaselineConsentResult::Ok);
  consent.reset();
  const auto intentPath = consentPath.substr(0, consentPath.size() - 8) + ".prepared";
  hal.files[intentPath].resize(COURSE_BASELINE_PUBLICATION_SIZE);
  ASSERT_TRUE(encodeCourseBaselinePublicationRecord(record, hal.files.at(intentPath)));
  hal.files["/tinta/original.pack"] = bytes;
  const auto initial = hal;
  auto parser = makeUniqueNoThrow<tinta::core::pack::Pack>();
  ASSERT_TRUE(parser);
  auto session = makeUniqueNoThrow<HalCourseBaselineArchiveSession>(record.reader, generation, record.request.owner,
                                                                    scratch, *parser, permission, nullptr);
  ASSERT_TRUE(session);
  for (unsigned fault = 0; fault < 9; ++fault) {
    SCOPED_TRACE(fault);
    hal = initial;
    auto input = record;
    if (fault == 0) hal.files[intentPath + ".tmp"] = hal.files.at(intentPath);
    if (fault == 1) hal.files[intentPath].pop_back();
    if (fault == 2) {
      auto foreign = record;
      foreign.request.reviewHash[0] ^= 1;
      ASSERT_TRUE(encodeCourseBaselinePublicationRecord(foreign, hal.files.at(intentPath)));
    }
    if (fault == 3) hal.files["/tinta/original.pack"].back() ^= 1;
    if (fault == 4) hal.files["/tinta/original.pack"].pop_back();
    if (fault == 5) hal.failSyncPath = "/tinta/original.pack";
    if (fault == 6) hal.failClosePath = "/tinta/original.pack";
    if (fault == 7) {
      // Deliberately construct matching immutable evidence for malformed bytes.
      auto& malformed = hal.files.at("/tinta/original.pack");
      malformed[0] ^= 1;
      SHA256(malformed.data(), malformed.size(), input.request.manifest.contentHash.data());
      ASSERT_TRUE(encodeCourseBaselineImportRequest(input.request, hal.files.at(consentPath)));
      ASSERT_TRUE(encodeCourseBaselinePublicationRecord(input, hal.files.at(intentPath)));
    }
    if (fault == 8) input.request.owner[0] ^= 0x80;
    const auto evidence = hal.files;
    const auto renames = hal.renames;
    EXPECT_FALSE(session->publishArchive(input, "/tinta/original.pack"));
    EXPECT_EQ(hal.files, evidence);
    EXPECT_EQ(hal.renames, renames);
  }
  hal = initial;
  EXPECT_TRUE(session->publishArchive(record, "/tinta/original.pack"));
  EXPECT_TRUE(session->verify(record));
}

TEST_F(HalCourseTransferTest, PreparedBaselineRecoveryRecognizesOnlyOwnArchiveAndUnchangedReviewedState) {
  CourseBaselinePublicationRecord record;
  std::string consentPath;
  prepareBaselineApproval(record.request, record.reader, consentPath);
  ASSERT_FALSE(HasFatalFailure());
  auto& hal = inventory_hal_test::state;
  auto permission = [](void*) { return true; };
  auto consent = makeUniqueNoThrow<HalCourseBaselineImportConsentStore>(record.reader, generation, record.request.owner,
                                                                        scratch, permission, nullptr);
  ASSERT_TRUE(consent);
  ASSERT_EQ(consent->approve(record.request, declaration), CourseBaselineConsentResult::Ok);
  consent.reset();
  hal.files["/tinta/original.pack"] = bytes;
  auto parser = makeUniqueNoThrow<tinta::core::pack::Pack>();
  ASSERT_TRUE(parser);
  auto session = makeUniqueNoThrow<HalCourseBaselineArchiveSession>(record.reader, generation, record.request.owner,
                                                                    scratch, *parser, permission, nullptr);
  ASSERT_TRUE(session);
  struct ArchiveCut {
    HalCourseBaselineArchiveSession* session;
    bool stop = true;
  } cut{session.get()};
  CourseBaselinePublicationHooks hooks{
      &cut,
      [](void* context, const CourseBaselinePublicationRecord& request, bool recovering) {
        return static_cast<ArchiveCut*>(context)->session->verifyPrepared(request, recovering);
      },
      [](void* context, const CourseBaselinePublicationRecord& request) {
        auto& cut = *static_cast<ArchiveCut*>(context);
        return cut.session->publishArchive(request, "/tinta/original.pack") && !cut.stop;
      },
      [](void* context, const CourseBaselinePublicationRecord& request) {
        return static_cast<ArchiveCut*>(context)->session->verify(request);
      }};
  auto publication = makeUniqueNoThrow<HalCourseBaselinePublicationStore>(
      record.reader, generation, record.request.owner, scratch, permission, nullptr);
  ASSERT_TRUE(publication);
  EXPECT_EQ(publication->publish(record, hooks), CourseBaselinePublicationResult::IoError);
  EXPECT_EQ(publication->published(), nullptr);
  const auto interrupted = hal.files;
  EXPECT_FALSE(session->verifyPrepared(record, false));
  ASSERT_TRUE(session->verifyPrepared(record, true));
  std::array<char, COURSE_STATE_PATH_SIZE> items{};
  ASSERT_TRUE(courseStatePath(record.request.manifest.logicalIdentity, "items.bin", items));
  hal.files[items.data()] = {71};
  const auto changed = hal.files;
  EXPECT_FALSE(session->verifyPrepared(record, true));
  EXPECT_EQ(hal.files, changed);
  hal.files = interrupted;
  auto foreign = std::find_if(hal.files.begin(), hal.files.end(), [](const auto& file) {
    return file.first.starts_with("/tinta/courses/") && file.first.ends_with(".ref");
  });
  ASSERT_NE(foreign, hal.files.end());
  auto foreignPath = foreign->first;
  foreignPath[foreignPath.size() - 5] = foreignPath[foreignPath.size() - 5] == '0' ? '1' : '0';
  hal.files[foreignPath] = foreign->second;
  const auto conflicting = hal.files;
  EXPECT_FALSE(session->verifyPrepared(record, true));
  EXPECT_EQ(hal.files, conflicting);
  hal.files = interrupted;
  cut.stop = false;
  ASSERT_EQ(publication->publish(record, hooks), CourseBaselinePublicationResult::Ok);
  ASSERT_NE(publication->published(), nullptr);
  EXPECT_EQ(hal.files.at(items.data()), interrupted.at(items.data()));
}

TEST_F(HalCourseTransferTest, PreparedBaselineRecoveryResumesProvenPendingReferencePrefixes) {
  CourseBaselinePublicationRecord record;
  std::string consentPath;
  prepareBaselineApproval(record.request, record.reader, consentPath);
  ASSERT_FALSE(HasFatalFailure());
  auto& hal = inventory_hal_test::state;
  auto permission = [](void*) { return true; };
  auto consent = makeUniqueNoThrow<HalCourseBaselineImportConsentStore>(record.reader, generation, record.request.owner,
                                                                        scratch, permission, nullptr);
  ASSERT_TRUE(consent);
  ASSERT_EQ(consent->approve(record.request, declaration), CourseBaselineConsentResult::Ok);
  consent.reset();
  hal.files["/tinta/original.pack"] = bytes;
  auto archive = makeUniqueNoThrow<HalCoursePackArchive>(scratch, permission, nullptr);
  ASSERT_TRUE(archive);
  ASSERT_EQ(archive->publish(record.request.manifest, "/tinta/original.pack"), CourseArchiveResult::Ok);
  const auto reference = std::string(archive->referencePath());
  const auto pending = reference.substr(0, reference.size() - 4) + ".tmp";
  archive.reset();
  const auto intentPath = consentPath.substr(0, consentPath.size() - 8) + ".prepared";
  hal.files[intentPath].resize(COURSE_BASELINE_PUBLICATION_SIZE);
  ASSERT_TRUE(encodeCourseBaselinePublicationRecord(record, hal.files.at(intentPath)));
  const auto complete = hal;
  auto parser = makeUniqueNoThrow<tinta::core::pack::Pack>();
  ASSERT_TRUE(parser);
  auto session = makeUniqueNoThrow<HalCourseBaselineArchiveSession>(record.reader, generation, record.request.owner,
                                                                    scratch, *parser, permission, nullptr);
  ASSERT_TRUE(session);
  CourseBaselinePublicationHooks hooks{session.get(), HalCourseBaselineArchiveSession::verifyPreparation,
                                       [](void* context, const CourseBaselinePublicationRecord& request) {
                                         return static_cast<HalCourseBaselineArchiveSession*>(context)->publishArchive(
                                             request, "/tinta/original.pack");
                                       },
                                       HalCourseBaselineArchiveSession::verifyPublication};
  auto publication = makeUniqueNoThrow<HalCourseBaselinePublicationStore>(
      record.reader, generation, record.request.owner, scratch, permission, nullptr);
  ASSERT_TRUE(publication);
  for (const size_t length : {size_t{0}, size_t{1}, COURSE_BINDING_SIZE / 2, COURSE_BINDING_SIZE}) {
    SCOPED_TRACE(length);
    hal = complete;
    hal.files.erase(reference);
    const auto& encoded = complete.files.at(reference);
    hal.files[pending] = {encoded.begin(), encoded.begin() + length};
    const auto evidence = hal.files;
    ASSERT_TRUE(session->verifyPrepared(record, true));
    EXPECT_FALSE(session->verifyPrepared(record, false));
    EXPECT_EQ(hal.files, evidence);
    ASSERT_EQ(publication->publish(record, hooks), CourseBaselinePublicationResult::Ok);
    EXPECT_NE(publication->published(), nullptr);
    EXPECT_FALSE(hal.files.contains(pending));
    EXPECT_EQ(hal.files.at(reference), encoded);
    for (const auto& [path, data] : complete.files) EXPECT_EQ(hal.files.at(path), data);
  }
  for (unsigned fault = 0; fault < 3; ++fault) {
    hal = complete;
    hal.files.erase(reference);
    hal.files[pending] = complete.files.at(reference);
    if (fault == 0) hal.files[pending][0] ^= 1;
    if (fault == 1) hal.files[reference] = complete.files.at(reference);
    if (fault == 2) hal.failClosePath = pending;
    const auto evidence = hal.files;
    EXPECT_FALSE(session->verifyPrepared(record, true));
    EXPECT_EQ(hal.files, evidence);
  }
}

TEST_F(HalCourseTransferTest, NativeBaselineInstallerValidatesBeforeInstallingAndRechecksCommittedEvidenceReadOnly) {
  CourseBaselineImportRequest request;
  Identity reader{};
  std::string consentPath;
  prepareBaselineApproval(request, reader, consentPath);
  ASSERT_FALSE(HasFatalFailure());
  auto& hal = inventory_hal_test::state;
  auto permission = [](void*) { return true; };
  struct Compatibility {
    bool approved = true;
    unsigned calls = 0;
  } compatibility;
  auto check = [](void* context, const CourseBaselinePublicationRecord&, const char*, std::span<uint8_t>) {
    auto& compatibility = *static_cast<Compatibility*>(context);
    ++compatibility.calls;
    return compatibility.approved;
  };
  auto installer = createHalCourseBaselineNativeInstaller(reader, generation, request.owner, scratch, permission,
                                                          nullptr, check, &compatibility);
  ASSERT_TRUE(installer);
  auto state = declaration.state;
  state.durableOffset = state.length;
  hal.files[TRANSFER_STAGE] = bytes;
  const auto unapproved = hal.files;
  EXPECT_FALSE(installer->prepare(COURSE_BASELINE_DESTINATION, TRANSFER_STAGE, request.manifest, state, scratch));
  EXPECT_EQ(hal.files, unapproved);
  auto consent = makeUniqueNoThrow<HalCourseBaselineImportConsentStore>(reader, generation, request.owner, scratch,
                                                                        permission, nullptr);
  ASSERT_TRUE(consent);
  ASSERT_EQ(consent->approve(request, declaration), CourseBaselineConsentResult::Ok);
  consent.reset();
  const auto approved = hal.files;
  compatibility.approved = false;
  EXPECT_FALSE(installer->prepare(COURSE_BASELINE_DESTINATION, TRANSFER_STAGE, request.manifest, state, scratch));
  EXPECT_EQ(hal.files, approved);
  compatibility.approved = true;
  ASSERT_TRUE(installer->prepare(COURSE_BASELINE_DESTINATION, TRANSFER_STAGE, request.manifest, state, scratch));
  EXPECT_EQ(hal.files, approved);
  for (unsigned fault = 0; fault < 4; ++fault) {
    auto foreign = state;
    if (fault == 0) foreign.owner[0] ^= 0x80;
    if (fault == 1) foreign.storageGeneration[0] ^= 0x80;
    if (fault == 2) foreign.transaction[0] ^= 0x80;
    if (fault == 3) --foreign.durableOffset;
    EXPECT_FALSE(installer->prepare(COURSE_BASELINE_DESTINATION, TRANSFER_STAGE, request.manifest, foreign, scratch));
    EXPECT_EQ(hal.files, approved);
  }
  ASSERT_TRUE(Storage.rename(TRANSFER_STAGE, COURSE_BASELINE_DESTINATION));
  state.phase = TransferPhase::Installing;
  ASSERT_TRUE(installer->metadata(COURSE_BASELINE_DESTINATION, request.manifest, state, scratch));
  EXPECT_EQ(hal.files.at(COURSE_BASELINE_DESTINATION), bytes);
  state.phase = TransferPhase::Committed;
  std::array<char, COURSE_STATE_PATH_SIZE> items{};
  ASSERT_TRUE(courseStatePath(request.manifest.logicalIdentity, "items.bin", items));
  hal.files[items.data()] = {89};
  const auto committed = hal.files;
  const auto calls = compatibility.calls;
  compatibility.approved = false;
  ASSERT_TRUE(installer->metadata(COURSE_BASELINE_DESTINATION, request.manifest, state, scratch));
  EXPECT_EQ(compatibility.calls, calls);
  EXPECT_EQ(hal.files, committed);
  const auto published = consentPath.substr(0, consentPath.size() - 8) + ".published";
  hal.files.erase(published);
  const auto missing = hal.files;
  EXPECT_FALSE(installer->metadata(COURSE_BASELINE_DESTINATION, request.manifest, state, scratch));
  EXPECT_EQ(hal.files, missing);
  EXPECT_EQ(compatibility.calls, calls);
}

TEST_F(HalCourseTransferTest, NativeBaselineInstallerAdmissionRequiresPermissionCompatibilityAndHeapReserve) {
  Identity reader{};
  reader[0] = 1;
  const auto original = inventory_hal_test::state.files;
  bool permitted = false;
  auto permission = [](void* context) { return *static_cast<bool*>(context); };
  auto compatible = [](void*, const CourseBaselinePublicationRecord&, const char*, std::span<uint8_t>) { return true; };
  EXPECT_FALSE(createHalCourseBaselineNativeInstaller(reader, generation, declaration.state.owner, scratch, permission,
                                                      &permitted, compatible, nullptr));
  permitted = true;
  EXPECT_FALSE(createHalCourseBaselineNativeInstaller(reader, generation, declaration.state.owner, scratch, permission,
                                                      &permitted, nullptr, nullptr));
  const auto available = companion_memory_test::internal;
  companion_memory_test::internal.freeBytes = 50 * 1024 + sizeof(HalCourseBaselineNativeInstaller);
  EXPECT_FALSE(createHalCourseBaselineNativeInstaller(reader, generation, declaration.state.owner, scratch, permission,
                                                      &permitted, compatible, nullptr));
  companion_memory_test::internal = available;
  companion_memory_test::internal.largestBlockBytes = sizeof(HalCourseBaselineNativeInstaller) - 1;
  EXPECT_FALSE(createHalCourseBaselineNativeInstaller(reader, generation, declaration.state.owner, scratch, permission,
                                                      &permitted, compatible, nullptr));
  companion_memory_test::internal = available;
  EXPECT_TRUE(createHalCourseBaselineNativeInstaller(reader, generation, declaration.state.owner, scratch, permission,
                                                     &permitted, compatible, nullptr));
  EXPECT_EQ(inventory_hal_test::state.files, original);
}

TEST_F(HalCourseTransferTest, BaselineParentTransferCommitsThroughNativeInstallerAndRecoversNewerLearnerState) {
  CourseBaselineImportRequest request;
  Identity reader{};
  std::string consentPath;
  prepareBaselineApproval(request, reader, consentPath);
  ASSERT_FALSE(HasFatalFailure());
  auto& hal = inventory_hal_test::state;
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  std::array<uint8_t, COURSE_BINDING_SIZE> binding{};
  ASSERT_EQ(encodeCourseBinding(declaration.manifest, binding), binding.size());
  hal.files[COURSE_BINDING_PATH] = {binding.begin(), binding.end()};
  const auto active = hal.files.at(ACTIVE_COURSE_PATH), activeBinding = hal.files.at(COURSE_BINDING_PATH);
  auto permission = [](void*) { return true; };
  auto consent = makeUniqueNoThrow<HalCourseBaselineImportConsentStore>(reader, generation, request.owner, scratch,
                                                                        permission, nullptr);
  ASSERT_TRUE(consent);
  auto approve = [](void* context, const CourseBaselineImportRequest& request, const TransferDeclaration& declaration) {
    return static_cast<HalCourseBaselineImportConsentStore*>(context)->approve(request, declaration) ==
           CourseBaselineConsentResult::Ok;
  };
  Transfer transfer(storage, scratch);
  ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
  ASSERT_EQ(beginCourseBaselineTransfer(transfer, request, generation, request.owner, approve, consent.get()),
            TransferResult::Ok);
  consent.reset();
  for (size_t offset = 0; offset < bytes.size();) {
    const auto count = std::min(size_t{1000}, bytes.size() - offset);
    ASSERT_EQ(transfer.append(request.transaction, request.owner, offset, std::span(bytes).subspan(offset, count)),
              TransferResult::Ok);
    offset += count;
  }
  EXPECT_EQ(transfer.commit(request.transaction, request.owner), TransferResult::Invalid);
  EXPECT_EQ(transfer.current()->phase, TransferPhase::Receiving);
  EXPECT_FALSE(hal.files.contains(COURSE_BASELINE_DESTINATION));
  unsigned compatibilityCalls = 0;
  auto compatible = [](void* context, const CourseBaselinePublicationRecord&, const char*, std::span<uint8_t>) {
    ++*static_cast<unsigned*>(context);
    return true;
  };
  auto installer = createHalCourseBaselineNativeInstaller(reader, generation, request.owner, scratch, permission,
                                                          nullptr, compatible, &compatibilityCalls);
  ASSERT_TRUE(installer);
  storage.setCourseBaselineInstaller(installer.get());
  ASSERT_EQ(transfer.commit(request.transaction, request.owner), TransferResult::Ok);
  ASSERT_EQ(transfer.current()->phase, TransferPhase::Committed);
  EXPECT_EQ(hal.files.at(COURSE_BASELINE_DESTINATION), bytes);
  EXPECT_EQ(hal.files.at(ACTIVE_COURSE_PATH), active);
  EXPECT_EQ(hal.files.at(COURSE_BINDING_PATH), activeBinding);
  std::array<char, COURSE_STATE_PATH_SIZE> items{};
  ASSERT_TRUE(courseStatePath(request.manifest.logicalIdentity, "items.bin", items));
  hal.files[items.data()] = {97};
  const auto complete = hal.files;
  const auto calls = compatibilityCalls;
  ASSERT_EQ(transfer.commit(request.transaction, request.owner), TransferResult::Ok);
  HalTransferStorage reopened;
  reopened.setCourseBaselineInstaller(installer.get());
  Transfer restored(reopened, scratch);
  ASSERT_EQ(restored.recover(generation), TransferResult::Ok);
  EXPECT_EQ(restored.current()->phase, TransferPhase::Committed);
  EXPECT_EQ(compatibilityCalls, calls);
  EXPECT_EQ(hal.files, complete);
  storage.setCourseBaselineInstaller(nullptr);
  reopened.setCourseBaselineInstaller(nullptr);
  EXPECT_EQ(transfer.commit(request.transaction, request.owner), TransferResult::IoError);
  EXPECT_EQ(hal.files, complete);
}

TEST_F(HalCourseTransferTest, NativeBaselineInstallerRechecksEvidenceBeforeRecoveringTornStages) {
  for (const auto phase : {CourseBaselinePublicationPhase::Prepared, CourseBaselinePublicationPhase::Published}) {
    for (unsigned fault = 0; fault < 5; ++fault) {
      SCOPED_TRACE(::testing::Message() << "phase=" << unsigned(phase) << " fault=" << fault);
      SetUp();
      CourseBaselinePublicationRecord record;
      std::string consentPath;
      prepareBaselineApproval(record.request, record.reader, consentPath);
      ASSERT_FALSE(HasFatalFailure());
      auto permission = [](void*) { return true; };
      auto consent = makeUniqueNoThrow<HalCourseBaselineImportConsentStore>(
          record.reader, generation, record.request.owner, scratch, permission, nullptr);
      ASSERT_TRUE(consent);
      ASSERT_EQ(consent->approve(record.request, declaration), CourseBaselineConsentResult::Ok);
      consent.reset();
      auto& hal = inventory_hal_test::state;
      hal.files[COURSE_BASELINE_DESTINATION] = bytes;
      const auto prefix = consentPath.substr(0, consentPath.size() - std::string_view(".consent").size());
      std::array<uint8_t, COURSE_BASELINE_PUBLICATION_SIZE> encoded{};
      ASSERT_TRUE(encodeCourseBaselinePublicationRecord(record, encoded));
      if (phase == CourseBaselinePublicationPhase::Published)
        hal.files[prefix + ".prepared"] = {encoded.begin(), encoded.end()};
      record.phase = phase;
      ASSERT_TRUE(encodeCourseBaselinePublicationRecord(record, encoded));
      const auto staged =
          prefix + (phase == CourseBaselinePublicationPhase::Prepared ? ".prepared.tmp" : ".published.tmp");
      hal.files[staged] = {encoded.begin(), encoded.begin() + 100};
      if (fault == 1) hal.files.at(consentPath).back() ^= 1;
      if (fault == 2 || fault == 3) {
        auto backup = std::find_if(hal.files.begin(), hal.files.end(), [](const auto& entry) {
          return entry.first.starts_with("/.crosspoint/companion/course-review-state-") && !entry.second.empty();
        });
        ASSERT_NE(backup, hal.files.end());
        if (fault == 2)
          hal.files.erase(backup);
        else
          backup->second.back() ^= 1;
      }
      if (fault == 4) {
        std::array<char, COURSE_STATE_PATH_SIZE> items{};
        ASSERT_TRUE(courseStatePath(record.request.manifest.logicalIdentity, "items.bin", items));
        ASSERT_TRUE(hal.files.contains(items.data()));
        ASSERT_FALSE(hal.files.at(items.data()).empty());
        hal.files.at(items.data()).back() ^= 1;
      }
      const auto evidence = hal.files;
      auto compatible = [](void*, const CourseBaselinePublicationRecord&, const char*, std::span<uint8_t>) {
        return true;
      };
      auto installer = createHalCourseBaselineNativeInstaller(record.reader, generation, record.request.owner, scratch,
                                                              permission, nullptr, compatible, nullptr);
      ASSERT_TRUE(installer);
      auto state = declaration.state;
      state.phase = TransferPhase::Installing;
      state.durableOffset = state.length;
      EXPECT_EQ(installer->metadata(COURSE_BASELINE_DESTINATION, declaration.manifest, state, scratch), fault == 0);
      if (fault != 0) {
        EXPECT_EQ(hal.files, evidence);
      } else {
        EXPECT_FALSE(hal.files.contains(staged));
        EXPECT_TRUE(hal.files.contains(prefix + ".prepared"));
        EXPECT_TRUE(hal.files.contains(prefix + ".published"));
        for (const auto& [path, data] : evidence) {
          if (path != staged) {
            EXPECT_EQ(hal.files.at(path), data);
          }
        }
        const auto completed = hal.files;
        EXPECT_TRUE(installer->metadata(COURSE_BASELINE_DESTINATION, declaration.manifest, state, scratch));
        EXPECT_EQ(hal.files, completed);
      }
    }
  }
}

TEST_F(HalCourseTransferTest, BaselineParentInstallingRecoversRenameAndPhaseAcknowledgementFailures) {
  CourseBaselineImportRequest request;
  Identity reader{};
  std::string consentPath;
  prepareBaselineApproval(request, reader, consentPath);
  ASSERT_FALSE(HasFatalFailure());
  auto& hal = inventory_hal_test::state;
  auto permission = [](void*) { return true; };
  auto consent = makeUniqueNoThrow<HalCourseBaselineImportConsentStore>(reader, generation, request.owner, scratch,
                                                                        permission, nullptr);
  ASSERT_TRUE(consent);
  ASSERT_EQ(consent->approve(request, declaration), CourseBaselineConsentResult::Ok);
  consent.reset();
  Transfer initial(storage, scratch);
  ASSERT_EQ(initial.recover(generation), TransferResult::Ok);
  ASSERT_EQ(initial.begin(declaration, COURSE_BASELINE_DESTINATION), TransferResult::Ok);
  for (size_t offset = 0; offset < bytes.size();) {
    const auto count = std::min(size_t{1000}, bytes.size() - offset);
    ASSERT_EQ(initial.append(request.transaction, request.owner, offset, std::span(bytes).subspan(offset, count)),
              TransferResult::Ok);
    offset += count;
  }
  const auto uploaded = hal;
  const auto prefix = consentPath.substr(0, consentPath.size() - 8);
  for (unsigned fault = 0; fault < 4; ++fault) {
    SCOPED_TRACE(fault);
    hal = uploaded;
    auto compatible = [](void*, const CourseBaselinePublicationRecord&, const char*, std::span<uint8_t>) {
      return true;
    };
    auto installer = createHalCourseBaselineNativeInstaller(reader, generation, request.owner, scratch, permission,
                                                            nullptr, compatible, nullptr);
    ASSERT_TRUE(installer);
    HalTransferStorage native;
    native.setCourseBaselineInstaller(installer.get());
    Transfer transfer(native, scratch);
    ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
    if (fault == 0) hal.failRename = hal.renames + 1;
    if (fault == 1) hal.failRenameAfter = hal.renames + 1;
    if (fault == 2) hal.failSyncPath = prefix + ".prepared.tmp";
    if (fault == 3) hal.failSyncPath = prefix + ".published.tmp";
    EXPECT_EQ(transfer.commit(request.transaction, request.owner), TransferResult::IoError);
    EXPECT_EQ(transfer.current()->phase, TransferPhase::Installing);
    for (const auto& [path, data] : uploaded.files) {
      if (path != TRANSFER_STAGE && !path.starts_with("/.crosspoint/companion/transfer-")) {
        EXPECT_EQ(hal.files.at(path), data);
      }
    }
    native.setCourseBaselineInstaller(nullptr);
    installer.reset();
    hal.failRename = hal.failRenameAfter = 0;
    hal.failSyncPath.clear();
    installer = createHalCourseBaselineNativeInstaller(reader, generation, request.owner, scratch, permission, nullptr,
                                                       compatible, nullptr);
    ASSERT_TRUE(installer);
    HalTransferStorage recoveredStorage;
    recoveredStorage.setCourseBaselineInstaller(installer.get());
    Transfer recovered(recoveredStorage, scratch);
    ASSERT_EQ(recovered.recover(generation), TransferResult::Ok);
    EXPECT_EQ(recovered.current()->phase, TransferPhase::Committed);
    EXPECT_EQ(hal.files.at(COURSE_BASELINE_DESTINATION), bytes);
    recoveredStorage.setCourseBaselineInstaller(nullptr);
  }
}

TEST_F(HalCourseTransferTest, NativeLearnerInspectionUsesVerifiedImmutableCopiesAndCompleteCandidatePack) {
  auto parser = makeUniqueNoThrow<tinta::core::pack::Pack>();
  ASSERT_TRUE(parser);
  ASSERT_EQ(parser->open(bytes.data(), bytes.size()), tinta::core::pack::PackStatus::Ok);
  ASSERT_GT(parser->itemCount(), 0u);
  const auto uid = parser->uidAt(0);
  parser->close();
  std::vector<uint8_t> items(1040, 0);
  for (unsigned copy = 0; copy < 2; ++copy) {
    auto* header = items.data() + copy * 512;
    std::memcpy(header, "TIS1", 4);
    binary_record::putU16(header + 4, 1);
    binary_record::putU16(header + 6, 80);
    binary_record::putU32(header + 8, copy + 1);
    binary_record::putU32(header + 12, 1);
    binary_record::putU32(header + 76, binary_record::crc32(header, 76));
  }
  tinta::core::ItemState::fresh(uid).encode(items.data() + 1024);
  tinta::core::Profile profile;
  auto& hal = inventory_hal_test::state;
  hal.files["/tinta/profile.bin"].resize(tinta::core::Profile::kEncodedSize);
  profile.encode(hal.files["/tinta/profile.bin"].data());
  std::array<char, COURSE_STATE_PATH_SIZE> usage{};
  ASSERT_TRUE(courseStatePath(declaration.manifest.logicalIdentity, "usage-0001.log", usage));
  hal.files[usage.data()] = {'T', 'U', 99};
  CourseBaselinePublicationRecord record;
  std::string consentPath;
  prepareBaselineApproval(record.request, record.reader, consentPath, items);
  ASSERT_FALSE(HasFatalFailure());
  bool permitted = true;
  const auto permission = [](void* context) { return *static_cast<bool*>(context); };
  auto consent = makeUniqueNoThrow<HalCourseBaselineImportConsentStore>(record.reader, generation, record.request.owner,
                                                                        scratch, permission, &permitted);
  ASSERT_TRUE(consent);
  ASSERT_EQ(consent->approve(record.request, declaration), CourseBaselineConsentResult::Ok);
  consent.reset();
  hal.files["/tinta/original.pack"] = bytes;
  std::array<char, COURSE_STATE_PATH_SIZE> current{};
  ASSERT_TRUE(courseStatePath(record.request.manifest.logicalIdentity, "items.bin", current));
  hal.files[current.data()] = {97};
  const auto preserved = hal;
  auto inspector = makeUniqueNoThrow<HalCourseBaselineLearnerInspection>(
      record.reader, generation, record.request.owner, scratch, *parser, CourseBaselineSessionLimits{8, 40, 200},
      permission, &permitted);
  ASSERT_TRUE(inspector);
  ASSERT_TRUE(inspector->inspect(record, "/tinta/original.pack"));
  EXPECT_EQ(hal.files, preserved.files);
  EXPECT_EQ(hal.renames, preserved.renames);
  EXPECT_FALSE(parser->isOpen());
  for (unsigned fault = 0; fault < 6; ++fault) {
    hal = preserved;
    auto request = record;
    if (fault == 0) request.reader[0] ^= 1;
    if (fault == 1) hal.files["/tinta/original.pack"][0] ^= 1;
    if (fault == 2) hal.failClosePath = "/tinta/original.pack";
    if (fault == 3) permitted = false;
    if (fault == 4) hal.files.erase(consentPath);
    if (fault == 5) {
      for (auto& [path, data] : hal.files) {
        if (path.starts_with("/.crosspoint/companion/course-review-state-") && data.size() == 1040) {
          data.back() ^= 1;
          break;
        }
      }
    }
    const auto evidence = hal.files;
    EXPECT_FALSE(inspector->inspect(request, "/tinta/original.pack")) << fault;
    EXPECT_EQ(hal.files, evidence);
    permitted = true;
  }
}

TEST_F(HalCourseTransferTest, NativeLearnerInspectionAuditsFrozenJournalMembershipWithoutChangingEvidence) {
  for (const bool missing : {false, true}) {
    SetUp();
    SCOPED_TRACE(missing);
    auto parser = makeUniqueNoThrow<tinta::core::pack::Pack>();
    ASSERT_TRUE(parser);
    ASSERT_EQ(parser->open(bytes.data(), bytes.size()), tinta::core::pack::PackStatus::Ok);
    const auto uid = parser->uidAt(0);
    parser->close();
    std::vector<uint8_t> items(1040, 0);
    for (unsigned copy = 0; copy < 2; ++copy) {
      auto* header = items.data() + copy * 512;
      std::memcpy(header, "TIS1", 4);
      binary_record::putU16(header + 4, 1);
      binary_record::putU16(header + 6, 80);
      binary_record::putU32(header + 8, copy + 1);
      binary_record::putU32(header + 12, 1);
      binary_record::putU32(header + 76, binary_record::crc32(header, 76));
    }
    tinta::core::ItemState::fresh(uid).encode(items.data() + 1024);
    CourseBaselinePublicationRecord record;
    std::string consentPath;
    prepareBaselineApproval(record.request, record.reader, consentPath, items);
    ASSERT_FALSE(HasFatalFailure());
    {
      HalTintaJournalStorage native;
      auto journal = makeUniqueNoThrow<TintaJournal>(native, scratch);
      ASSERT_TRUE(journal);
      ASSERT_EQ(journal->open(), TintaJournalResult::Ok);
      TintaBody body;
      body.kind = EventKind::Star;
      body.course = declaration.manifest.logicalIdentity;
      body.uid = missing ? UINT32_MAX - 1 : uid;
      body.enabled = true;
      std::array<uint8_t, MAX_TINTA_BODY_SIZE> encoded{};
      const auto length = encodeTintaBody(body, encoded);
      ASSERT_NE(length, 0u);
      SyncEvent event;
      event.identity = {declaration.state.owner, 1, 1};
      event.storageGeneration = generation;
      event.kind = body.kind;
      event.resource = declaration.manifest.contentHash;
      ASSERT_TRUE(native.digest(std::span(encoded).first(length), event.bodyHash));
      ASSERT_EQ(journal->append(event, std::span(encoded).first(length)), TintaJournalResult::Ok);
      ASSERT_TRUE(native.close());
    }
    const auto permitted = [](void*) { return true; };
    auto capture = makeUniqueNoThrow<HalCourseBaselineReviewCapture>(scratch, permitted, nullptr);
    ASSERT_TRUE(capture);
    ASSERT_EQ(capture->capture(record.reader, generation, record.request.manifest.logicalIdentity),
              CourseBaselineReviewResult::Ok);
    ASSERT_NE(capture->hash(), nullptr);
    record.request.reviewHash = *capture->hash();
    const std::vector<uint8_t> review(capture->bytes().begin(), capture->bytes().end());
    capture.reset();
    auto store = makeUniqueNoThrow<HalCourseBaselineReviewStore>(
        std::span(scratch).subspan(COURSE_BASELINE_REVIEW_MAX_SIZE), permitted, nullptr);
    ASSERT_TRUE(store);
    ASSERT_EQ(store->publish(review, record.request.reviewHash), CourseBaselineReviewStoreResult::Ok);
    store.reset();
    auto consent = makeUniqueNoThrow<HalCourseBaselineImportConsentStore>(
        record.reader, generation, record.request.owner, scratch, permitted, nullptr);
    ASSERT_TRUE(consent);
    ASSERT_EQ(consent->approve(record.request, declaration), CourseBaselineConsentResult::Ok);
    consent.reset();
    auto& hal = inventory_hal_test::state;
    hal.files["/tinta/original.pack"] = bytes;
    const auto retained = hal.files;
    auto inspector = createHalCourseBaselineLearnerInspection(record.reader, generation, record.request.owner, scratch,
                                                              *parser, {8, 40, 200}, permitted, nullptr);
    ASSERT_TRUE(inspector);
    EXPECT_EQ(inspector->inspect(record, "/tinta/original.pack"), !missing);
    EXPECT_FALSE(parser->isOpen());
    for (const auto& [path, data] : retained) EXPECT_EQ(hal.files.at(path), data);
  }
}

TEST_F(HalCourseTransferTest, NativeLearnerInspectionProvesCanonicalReceiptFromFrozenCohort) {
  for (unsigned fault = 0; fault < 11; ++fault) {
    SetUp();
    SCOPED_TRACE(fault);
    CourseBaselinePublicationRecord record;
    std::string consentPath;
    prepareBaselineApproval(record.request, record.reader, consentPath);
    ASSERT_FALSE(HasFatalFailure());
    auto& hal = inventory_hal_test::state;
    auto original = makeUniqueNoThrow<tinta::core::pack::Pack>();
    ASSERT_TRUE(original);
    ASSERT_EQ(original->open(bytes.data(), bytes.size()), tinta::core::pack::PackStatus::Ok);
    const auto uid = original->uidAt(0);
    original.reset();
    Digest frontier{};
    {
      HalTintaJournalStorage native;
      auto journal = makeUniqueNoThrow<TintaJournal>(native, scratch);
      ASSERT_TRUE(journal);
      ASSERT_EQ(journal->open(), TintaJournalResult::Ok);
      TintaBody body;
      body.kind = EventKind::Star;
      body.course = record.request.manifest.logicalIdentity;
      body.uid = uid;
      body.enabled = true;
      std::array<uint8_t, MAX_TINTA_BODY_SIZE> encoded{};
      const auto encodedSize = encodeTintaBody(body, encoded);
      ASSERT_NE(encodedSize, 0u);
      SyncEvent event;
      event.identity = {record.request.owner, 1, 1};
      event.storageGeneration = generation;
      event.kind = body.kind;
      event.resource = record.request.manifest.contentHash;
      ASSERT_TRUE(native.digest(std::span(encoded).first(encodedSize), event.bodyHash));
      ASSERT_EQ(journal->append(event, std::span(encoded).first(encodedSize)), TintaJournalResult::Ok);
      ASSERT_TRUE(native.close());
      auto audit = makeUniqueNoThrow<HalJournalCausalAuditSession>();
      ASSERT_TRUE(audit);
      ASSERT_TRUE(audit->run(&frontier));
    }
    auto working = makeUniqueNoThrow<HalTintaReplayStore>(TintaReplayStoreTarget::BaselineProof);
    ASSERT_TRUE(working);
    ASSERT_TRUE(working->begin(record.request.manifest.logicalIdentity));
    auto starred = tinta::core::ItemState::fresh(uid);
    starred.flags = tinta::core::item_flag::kStarred;
    ASSERT_TRUE(working->putItem(starred));
    auto output = makeUniqueNoThrow<HalTintaReplayExport>(TintaReplayExportTarget::BaselineProof);
    ASSERT_TRUE(output);
    ASSERT_TRUE(output->run(*working, record.request.manifest.logicalIdentity, 0, scratch));
    for (unsigned at = 0; at < 5; ++at) {
      std::array<char, COURSE_STATE_PATH_SIZE> proof{}, active{};
      ASSERT_TRUE(tintaReplayExportPath(record.request.manifest.logicalIdentity, static_cast<TintaDerivedFile>(at),
                                        TintaReplayExportTarget::BaselineProof, proof));
      ASSERT_TRUE(courseStatePath(record.request.manifest.logicalIdentity, TINTA_DERIVED_FILE_NAMES[at][0], active));
      hal.files[active.data()] = hal.files.at(proof.data());
    }
    Identity snapshot{1};
    ASSERT_TRUE(output->manifest(generation, record.request.manifest.contentHash, frontier, snapshot, 1, scratch));
    std::array<char, COURSE_STATE_PATH_SIZE> receiptPath{};
    ASSERT_TRUE(
        tintaDerivedRecordPath(record.request.manifest.logicalIdentity, TintaDerivedRecord::Receipt, receiptPath));
    hal.files[receiptPath.data()] = {scratch.begin(), scratch.begin() + TINTA_DERIVED_MANIFEST_SIZE};
    Digest savedSnapshot{};
    const auto& manifestBytes = hal.files.at(receiptPath.data());
    ASSERT_NE(SHA256(manifestBytes.data(), manifestBytes.size(), savedSnapshot.data()), nullptr);
    std::vector<uint8_t> session(98, 0);
    std::memcpy(session.data(), "TSES", 4);
    binary_record::putU16(session.data() + 4, 3);
    session[6] = session[7] = 1;
    binary_record::putU16(session.data() + 8, 52);
    binary_record::putU16(session.data() + 10 + 16, 32);
    auto* queue = session.data() + 30;
    std::memcpy(queue, "TSQ1", 4);
    binary_record::putU16(queue + 4, 2);
    binary_record::putU32(queue + 28, binary_record::crc32(queue, 28));
    std::copy(savedSnapshot.begin(), savedSnapshot.end(), session.begin() + 62);
    if (fault == 1) session[62] ^= 1;
    binary_record::putU32(session.data() + 94, binary_record::crc32(session.data(), 94));
    std::array<char, COURSE_STATE_PATH_SIZE> sessionPath{};
    ASSERT_TRUE(courseStatePath(record.request.manifest.logicalIdentity, "session.bin", sessionPath));
    hal.files[sessionPath.data()] = session;
    if (fault == 2) {
      auto& corrupted = hal.files.at(receiptPath.data());
      corrupted[136] ^= 1;
      binary_record::putU32(corrupted.data() + 328, binary_record::crc32(corrupted.data(), 328));
    }
    if (fault == 3) {
      std::array<char, COURSE_STATE_PATH_SIZE> changedPath{};
      ASSERT_TRUE(courseStatePath(record.request.manifest.logicalIdentity, "items.bin", changedPath));
      auto& changed = hal.files.at(changedPath.data());
      for (const size_t at : {size_t(0), size_t(512)}) {
        binary_record::putU16(changed.data() + at + 20, 1);
        binary_record::putU32(changed.data() + at + 76, binary_record::crc32(changed.data() + at, 76));
      }
    }
    if (fault == 4) {
      hal.files.erase(receiptPath.data());
      for (const auto name : {"lessons.bin", "readings.bin"}) {
        std::array<char, COURSE_STATE_PATH_SIZE> removed{};
        ASSERT_TRUE(courseStatePath(record.request.manifest.logicalIdentity, name, removed));
        hal.files.erase(removed.data());
      }
    }
    if (fault == 5) {
      std::array<char, COURSE_STATE_PATH_SIZE> removed{};
      ASSERT_TRUE(courseStatePath(record.request.manifest.logicalIdentity, "days.bin", removed));
      hal.files.erase(removed.data());
    }
    if (fault >= 6 && fault <= 9) {
      static constexpr size_t BINDING_OFFSETS[] = {4, 20, 52, 100};
      auto& changed = hal.files.at(receiptPath.data());
      changed[BINDING_OFFSETS[fault - 6]] ^= 0x80;
      binary_record::putU32(changed.data() + 328, binary_record::crc32(changed.data(), 328));
      TintaDerivedManifestView valid;
      ASSERT_TRUE(valid.decode(changed));
    }
    if (fault == 10) {
      auto& changed = hal.files.at(sessionPath.data());
      changed[30] = 'X';
      binary_record::putU32(changed.data() + 58, binary_record::crc32(changed.data() + 30, 28));
      binary_record::putU32(changed.data() + 94, binary_record::crc32(changed.data(), 94));
    }
    output.reset();
    working.reset();
    auto permission = [](void*) { return true; };
    auto capture = makeUniqueNoThrow<HalCourseBaselineReviewCapture>(scratch, permission, nullptr);
    ASSERT_TRUE(capture);
    ASSERT_EQ(capture->capture(record.reader, generation, record.request.manifest.logicalIdentity),
              CourseBaselineReviewResult::Ok);
    record.request.reviewHash = *capture->hash();
    const std::vector<uint8_t> frozen(capture->bytes().begin(), capture->bytes().end());
    capture.reset();
    auto reviewStore = makeUniqueNoThrow<HalCourseBaselineReviewStore>(
        std::span(scratch).subspan(COURSE_BASELINE_REVIEW_MAX_SIZE), permission, nullptr);
    ASSERT_TRUE(reviewStore);
    ASSERT_EQ(reviewStore->publish(frozen, record.request.reviewHash), CourseBaselineReviewStoreResult::Ok);
    reviewStore.reset();
    auto consent = makeUniqueNoThrow<HalCourseBaselineImportConsentStore>(
        record.reader, generation, record.request.owner, scratch, permission, nullptr);
    ASSERT_TRUE(consent);
    ASSERT_EQ(consent->approve(record.request, declaration), CourseBaselineConsentResult::Ok);
    consent.reset();
    hal.files["/tinta/original.pack"] = bytes;
    const auto retained = hal.files;
    auto parser = makeUniqueNoThrow<tinta::core::pack::Pack>();
    ASSERT_TRUE(parser);
    auto inspection = createHalCourseBaselineLearnerInspection(record.reader, generation, record.request.owner, scratch,
                                                               *parser, {8, 40, 200}, permission, nullptr);
    ASSERT_TRUE(inspection);
    EXPECT_EQ(inspection->sessionRequiresRebuild(), std::nullopt);
    EXPECT_EQ(inspection->inspect(record, "/tinta/original.pack"), fault < 2);
    if (fault < 2) {
      ASSERT_TRUE(inspection->sessionRequiresRebuild().has_value());
      EXPECT_EQ(*inspection->sessionRequiresRebuild(), fault == 1);
    } else {
      EXPECT_EQ(inspection->sessionRequiresRebuild(), std::nullopt);
    }
    for (const auto& [path, data] : retained) EXPECT_EQ(hal.files.at(path), data);
  }
}

TEST_F(HalCourseTransferTest, NativeLearnerInspectionRejectsSemanticallyInvalidButHashVerifiedReview) {
  for (unsigned fault = 0; fault < 3; ++fault) {
    SetUp();
    SCOPED_TRACE(fault);
    auto parser = makeUniqueNoThrow<tinta::core::pack::Pack>();
    ASSERT_TRUE(parser);
    ASSERT_EQ(parser->open(bytes.data(), bytes.size()), tinta::core::pack::PackStatus::Ok);
    const auto uid = parser->uidAt(0);
    parser->close();
    std::vector<uint8_t> items(1040, 0);
    for (unsigned copy = 0; copy < 2; ++copy) {
      auto* header = items.data() + copy * 512;
      std::memcpy(header, "TIS1", 4);
      binary_record::putU16(header + 4, 1);
      binary_record::putU16(header + 6, 80);
      binary_record::putU32(header + 8, copy + 1);
      binary_record::putU32(header + 12, 1);
      binary_record::putU32(header + 76, binary_record::crc32(header, 76));
    }
    tinta::core::ItemState::fresh(fault == 0 ? 0xfffffffe : uid).encode(items.data() + 1024);
    if (fault == 1) items[1035] = 0x80;
    auto& hal = inventory_hal_test::state;
    if (fault == 2) {
      std::array<char, COURSE_STATE_PATH_SIZE> unknown{};
      ASSERT_TRUE(courseStatePath(declaration.manifest.logicalIdentity, "future-progress.bin", unknown));
      hal.files[unknown.data()] = {1, 2, 3};
    }
    CourseBaselinePublicationRecord record;
    std::string consentPath;
    prepareBaselineApproval(record.request, record.reader, consentPath, items);
    ASSERT_FALSE(HasFatalFailure());
    auto permission = [](void*) { return true; };
    auto consent = makeUniqueNoThrow<HalCourseBaselineImportConsentStore>(
        record.reader, generation, record.request.owner, scratch, permission, nullptr);
    ASSERT_TRUE(consent);
    ASSERT_EQ(consent->approve(record.request, declaration), CourseBaselineConsentResult::Ok);
    consent.reset();
    hal.files["/tinta/original.pack"] = bytes;
    const auto evidence = hal.files;
    auto inspection = createHalCourseBaselineLearnerInspection(record.reader, generation, record.request.owner, scratch,
                                                               *parser, {8, 40, 200}, permission, nullptr);
    ASSERT_TRUE(inspection);
    EXPECT_FALSE(inspection->inspect(record, "/tinta/original.pack"));
    EXPECT_EQ(hal.files, evidence);
  }
}

TEST_F(HalCourseTransferTest, BaselineTransferPublishesWithNativeLearnerCompatibilityInsteadOfModeledCallback) {
  auto parser = makeUniqueNoThrow<tinta::core::pack::Pack>();
  ASSERT_TRUE(parser);
  ASSERT_EQ(parser->open(bytes.data(), bytes.size()), tinta::core::pack::PackStatus::Ok);
  const auto uid = parser->uidAt(0);
  parser->close();
  std::vector<uint8_t> items(1040, 0);
  for (unsigned copy = 0; copy < 2; ++copy) {
    auto* header = items.data() + copy * 512;
    std::memcpy(header, "TIS1", 4);
    binary_record::putU16(header + 4, 1);
    binary_record::putU16(header + 6, 80);
    binary_record::putU32(header + 8, copy + 1);
    binary_record::putU32(header + 12, 1);
    binary_record::putU32(header + 76, binary_record::crc32(header, 76));
  }
  tinta::core::ItemState::fresh(uid).encode(items.data() + 1024);
  CourseBaselineImportRequest request;
  Identity reader{};
  std::string consentPath;
  prepareBaselineApproval(request, reader, consentPath, items);
  ASSERT_FALSE(HasFatalFailure());
  bool exclusiveWorkspace = true;
  auto permission = [](void* context) { return *static_cast<bool*>(context); };
  auto parentWorkspace = std::span(scratch).subspan(TRANSFER_OFFSET);
  ASSERT_EQ(parentWorkspace.size(), TRANSFER_SCRATCH_SIZE);
  Transfer transfer(storage, parentWorkspace);
  ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
  ASSERT_EQ(beginHalCourseBaselineImport(transfer, reader, generation, request.owner, request, scratch, permission,
                                         &exclusiveWorkspace),
            TransferResult::Ok);
  std::unique_ptr<HalCourseBaselineImportSession> session;
  for (size_t at = 0; at < bytes.size();) {
    const auto count = std::min<size_t>(1000, bytes.size() - at);
    ASSERT_EQ(transfer.append(request.transaction, request.owner, at, std::span(bytes).subspan(at, count)),
              TransferResult::Ok);
    at += count;
  }
  std::array<char, COURSE_STATE_PATH_SIZE> current{};
  ASSERT_TRUE(courseStatePath(request.manifest.logicalIdentity, "items.bin", current));
  auto& hal = inventory_hal_test::state;
  const auto beforeRefusal = hal.files;
  const std::vector<uint8_t> queuedArea(scratch.begin(), scratch.begin() + TRANSFER_OFFSET);
  exclusiveWorkspace = false;
  EXPECT_EQ(commitHalCourseBaselineImport(transfer, storage, reader, generation, request.owner, request.transaction,
                                          scratch, parentWorkspace, {8, 40, 200}, permission, &exclusiveWorkspace),
            TransferResult::Unauthorized);
  EXPECT_EQ(hal.files, beforeRefusal);
  EXPECT_TRUE(std::equal(queuedArea.begin(), queuedArea.end(), scratch.begin()));
  exclusiveWorkspace = true;
  hal.failRename = hal.renames + 1;
  ASSERT_EQ(commitHalCourseBaselineImport(transfer, storage, reader, generation, request.owner, request.transaction,
                                          scratch, parentWorkspace, {8, 40, 200}, permission, &exclusiveWorkspace),
            TransferResult::IoError);
  ASSERT_NE(transfer.current(), nullptr);
  ASSERT_EQ(transfer.current()->phase, TransferPhase::Installing);
  hal.failRename = 0;
  storage.setCourseBaselineInstaller(nullptr);
  session.reset();
  const auto interrupted = hal.files;
  Transfer recovered(storage, parentWorkspace);
  ASSERT_EQ(recovered.recover(generation, TransferRecoveryMode::InspectJournal), TransferResult::Ok);
  ASSERT_NE(recovered.current(), nullptr);
  ASSERT_NE(recovered.contentManifest(), nullptr);
  EXPECT_EQ(recovered.current()->owner, request.owner);
  EXPECT_EQ(*recovered.contentManifest(), request.manifest);
  EXPECT_EQ(recovered.destination(), COURSE_BASELINE_DESTINATION);
  EXPECT_EQ(hal.files, interrupted);
  EXPECT_EQ(recovered.commit(request.transaction, request.owner), TransferResult::NoTransaction);
  BaselineRecoveryIdentities identities(reader, generation);
  BaselineRecoveryPairings pairingStorage;
  Pairings pairings(pairingStorage);
  ASSERT_EQ(pairings.load(scratch), PairingResult::Ok);
  PairingSecret secret{};
  secret[0] = 12;
  PairingPeer peer{};
  peer[0] = 13;
  ASSERT_EQ(pairings.add(request.owner, secret, peer, scratch), PairingResult::Ok);
  IdentityState identity{reader, generation, 1};
  ASSERT_TRUE(attachHalCourseBaselineRecovery(recovered, storage, identities, identity, pairings, scratch,
                                              parentWorkspace, session, permission, &exclusiveWorkspace));
  ASSERT_TRUE(session);
  EXPECT_EQ(hal.files, interrupted);
  EXPECT_EQ(identities.writes, 0U);
  EXPECT_EQ(pairingStorage.writes, 1U);
  ASSERT_EQ(recovered.recover(generation), TransferResult::Ok);
  ASSERT_NE(recovered.current(), nullptr);
  EXPECT_EQ(recovered.current()->phase, TransferPhase::Committed);
  EXPECT_EQ(hal.files.at(current.data()), items);
  storage.setCourseBaselineInstaller(nullptr);
  session.reset();
  hal.files[current.data()] = {97};
  const auto after = hal.files;
  ASSERT_EQ(commitHalCourseBaselineImport(recovered, storage, reader, generation, request.owner, request.transaction,
                                          scratch, parentWorkspace, {8, 40, 200}, permission, &exclusiveWorkspace),
            TransferResult::Ok);
  EXPECT_EQ(hal.files, after);
  EXPECT_FALSE(storage.hasCourseBaselineInstaller());
  storage.setCourseBaselineInstaller(nullptr);
}

TEST_F(HalCourseTransferTest, NativeLearnerInspectionAdmissionPreservesHeapReserveAndRefusesIncompleteLimits) {
  auto parser = makeUniqueNoThrow<tinta::core::pack::Pack>();
  ASSERT_TRUE(parser);
  Identity reader{};
  reader[0] = 51;
  bool permitted = true;
  auto permission = [](void* context) { return *static_cast<bool*>(context); };
  const auto evidence = inventory_hal_test::state.files;
  EXPECT_FALSE(createHalCourseBaselineLearnerInspection(reader, generation, declaration.state.owner, scratch, *parser,
                                                        {8, 40, 0}, permission, &permitted));
  EXPECT_FALSE(createHalCourseBaselineLearnerInspection(reader, generation, declaration.state.owner,
                                                        std::span(scratch).first(COURSE_BASELINE_REVIEW_MAX_SIZE),
                                                        *parser, {8, 40, 200}, permission, &permitted));
  permitted = false;
  EXPECT_FALSE(createHalCourseBaselineLearnerInspection(reader, generation, declaration.state.owner, scratch, *parser,
                                                        {8, 40, 200}, permission, &permitted));
  permitted = true;
  const auto memory = companion_memory_test::internal;
  companion_memory_test::internal.freeBytes = 50 * 1024 + sizeof(HalCourseBaselineLearnerInspection);
  EXPECT_FALSE(createHalCourseBaselineLearnerInspection(reader, generation, declaration.state.owner, scratch, *parser,
                                                        {8, 40, 200}, permission, &permitted));
  companion_memory_test::internal = memory;
  companion_memory_test::internal.largestBlockBytes = sizeof(HalCourseBaselineLearnerInspection) - 1;
  EXPECT_FALSE(createHalCourseBaselineLearnerInspection(reader, generation, declaration.state.owner, scratch, *parser,
                                                        {8, 40, 200}, permission, &permitted));
  companion_memory_test::internal = memory;
  EXPECT_TRUE(createHalCourseBaselineLearnerInspection(reader, generation, declaration.state.owner, scratch, *parser,
                                                       {8, 40, 200}, permission, &permitted));
  EXPECT_EQ(inventory_hal_test::state.files, evidence);
}

TEST_F(HalCourseTransferTest, BaselineImportSessionAdmissionIncludesConcurrentReplayAllocations) {
  Identity reader{};
  reader[0] = 51;
  bool permitted = true;
  auto permission = [](void* context) { return *static_cast<bool*>(context); };
  const auto memory = companion_memory_test::internal;
  const auto files = inventory_hal_test::state.files;
  constexpr size_t peak = sizeof(HalCourseBaselineImportSession) + sizeof(HalHistoricalCourseHistory);
  companion_memory_test::internal.freeBytes = 50 * 1024 + peak;
  EXPECT_FALSE(createHalCourseBaselineImportSession(reader, generation, declaration.state.owner, scratch, {8, 40, 200},
                                                    permission, &permitted));
  const auto replayPeak = sizeof(HalCourseBaselineImportSession) + sizeof(HalCourseBaselineReplaySession) +
                          sizeof(HalCourseBaselineReviewedJournalAudit) + sizeof(TintaReplayReducer) +
                          HalJournalCausalAuditSession::replayWorkspaceBytes();
  ASSERT_GT(replayPeak, peak);
  companion_memory_test::internal.freeBytes = 50 * 1024 + replayPeak;
  EXPECT_FALSE(createHalCourseBaselineImportSession(reader, generation, declaration.state.owner, scratch, {8, 40, 200},
                                                    permission, &permitted));
  companion_memory_test::internal.freeBytes += 1;
  EXPECT_TRUE(createHalCourseBaselineImportSession(reader, generation, declaration.state.owner, scratch, {8, 40, 200},
                                                   permission, &permitted));
  companion_memory_test::internal = memory;
  companion_memory_test::internal.largestBlockBytes = sizeof(HalCourseBaselineImportSession) - 1;
  EXPECT_FALSE(createHalCourseBaselineImportSession(reader, generation, declaration.state.owner, scratch, {8, 40, 200},
                                                    permission, &permitted));
  companion_memory_test::internal = memory;
  permitted = false;
  EXPECT_FALSE(createHalCourseBaselineImportSession(reader, generation, declaration.state.owner, scratch, {8, 40, 200},
                                                    permission, &permitted));
  permitted = true;
  EXPECT_FALSE(createHalCourseBaselineImportSession({}, generation, declaration.state.owner, scratch, {8, 40, 200},
                                                    permission, &permitted));
  EXPECT_FALSE(createHalCourseBaselineImportSession(reader, generation, declaration.state.owner, scratch, {8, 40, 0},
                                                    permission, &permitted));
  EXPECT_TRUE(createHalCourseBaselineImportSession(reader, generation, declaration.state.owner, scratch, {8, 40, 200},
                                                   permission, &permitted));
  EXPECT_EQ(inventory_hal_test::state.files, files);
}

TEST_F(HalCourseTransferTest, BaselineImportSessionRequiresParentWorkspaceInsideExclusiveLoan) {
  Identity reader{};
  reader[0] = 51;
  auto permitted = [](void*) { return true; };
  std::array<uint8_t, TRANSFER_JOURNAL_SIZE> foreign{};
  EXPECT_FALSE(createHalCourseBaselineImportSession(reader, generation, declaration.state.owner, scratch, {8, 40, 200},
                                                    permitted, nullptr, foreign));
  EXPECT_FALSE(createHalCourseBaselineImportSession(reader, generation, declaration.state.owner, scratch, {8, 40, 200},
                                                    permitted, nullptr,
                                                    std::span(scratch).last(TRANSFER_JOURNAL_SIZE - 1)));
  auto parent = std::span(scratch).subspan(TRANSFER_OFFSET);
  auto session = createHalCourseBaselineImportSession(reader, generation, declaration.state.owner, scratch,
                                                      {8, 40, 200}, permitted, nullptr, parent);
  ASSERT_TRUE(session);
  const auto files = inventory_hal_test::state.files;
  EXPECT_FALSE(session->installer()->prepare(COURSE_BASELINE_DESTINATION, TRANSFER_STAGE, declaration.manifest,
                                             declaration.state, scratch));
  EXPECT_FALSE(
      session->installer()->metadata(COURSE_BASELINE_DESTINATION, declaration.manifest, declaration.state, foreign));
  EXPECT_EQ(inventory_hal_test::state.files, files);
}

TEST_F(HalCourseTransferTest,
       BaselineJournalReadinessProvesAbsenceAndRefusesIncompleteEvidenceWithoutCreatingAuthority) {
  ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
  auto permitted = [](void*) { return true; };
  auto& hal = inventory_hal_test::state;
  const auto absent = hal.files;
  ASSERT_TRUE(prepareHalCourseBaselineJournal(permitted, nullptr));
  EXPECT_EQ(hal.files, absent);
  EXPECT_FALSE(hal.directories.contains(TINTA_JOURNAL_DIRECTORY));
  ASSERT_TRUE(Storage.ensureDirectoryExists(TINTA_JOURNAL_DIRECTORY));
  const auto empty = hal.files;
  EXPECT_FALSE(prepareHalCourseBaselineJournal(permitted, nullptr));
  EXPECT_EQ(hal.files, empty);
  EXPECT_FALSE(hal.files.contains(TINTA_JOURNAL_EVENTS));
  hal.files[TINTA_JOURNAL_EVENTS] = {};
  const auto headerless = hal.files;
  EXPECT_FALSE(prepareHalCourseBaselineJournal(permitted, nullptr));
  EXPECT_EQ(hal.files, headerless);
  EXPECT_FALSE(hal.files.contains(TINTA_JOURNAL_HEADER_A));
  hal.files[TINTA_JOURNAL_HEADER_A] = {1};
  const auto corrupt = hal.files;
  EXPECT_FALSE(prepareHalCourseBaselineJournal(permitted, nullptr));
  EXPECT_EQ(hal.files, corrupt);
}

TEST_F(HalCourseTransferTest, BaselineJournalReadinessAuditsExistingAuthorityAndRecoversOnlyUncommittedTail) {
  ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
  {
    HalTintaJournalStorage native;
    TintaJournal journal(native, std::span(scratch).first(TintaJournal::EXTENDED_RECORD_SIZE));
    ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
    ASSERT_TRUE(native.close());
  }
  auto& hal = inventory_hal_test::state;
  const char* headerPath = hal.files.contains(TINTA_JOURNAL_HEADER_A) ? TINTA_JOURNAL_HEADER_A : TINTA_JOURNAL_HEADER_B;
  const char* otherHeader = headerPath == TINTA_JOURNAL_HEADER_A ? TINTA_JOURNAL_HEADER_B : TINTA_JOURNAL_HEADER_A;
  ASSERT_TRUE(hal.files.contains(headerPath));
  const auto header = hal.files.at(headerPath);
  hal.files[TINTA_JOURNAL_EVENTS] = {99};
  auto permitted = [](void*) { return true; };
  ASSERT_TRUE(prepareHalCourseBaselineJournal(permitted, nullptr));
  EXPECT_TRUE(hal.files.at(TINTA_JOURNAL_EVENTS).empty());
  EXPECT_EQ(hal.files.at(headerPath), header);
  EXPECT_FALSE(hal.files.contains(otherHeader));
  const auto recovered = hal.files;
  EXPECT_FALSE(prepareHalCourseBaselineJournal([](void*) { return false; }, nullptr));
  EXPECT_EQ(hal.files, recovered);
}

TEST_F(HalCourseTransferTest, ReviewedJournalSnapshotUsesNativeHeadersAndNeverRepairsImmutableCopies) {
  ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
  {
    HalTintaJournalStorage native;
    TintaJournal journal(native, scratch);
    ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
    ASSERT_TRUE(native.close());
  }
  auto& hal = inventory_hal_test::state;
  const auto canonical = hal.files;
  auto hash = [](void*, std::span<const uint8_t> input, Digest& output) {
    return SHA256(input.data(), input.size(), output.data()) != nullptr;
  };
  for (unsigned fault = 0; fault < 4; ++fault) {
    SCOPED_TRACE(fault);
    hal.files = canonical;
    std::ifstream fixture(std::string(COMPANION_FIXTURE_DIR) + "CourseBaselineReview-v1.fixture", std::ios::binary);
    std::vector<uint8_t> review{std::istreambuf_iterator<char>(fixture), std::istreambuf_iterator<char>()};
    ASSERT_EQ(review.size(), 608u);
    review[5] = 1;
    std::array<std::vector<uint8_t>, 3> copies;
    std::array<bool, 3> present{true, canonical.contains(TINTA_JOURNAL_HEADER_A),
                                canonical.contains(TINTA_JOURNAL_HEADER_B)};
    copies[0] = canonical.at(TINTA_JOURNAL_EVENTS);
    if (present[1]) copies[1] = canonical.at(TINTA_JOURNAL_HEADER_A);
    if (present[2]) copies[2] = canonical.at(TINTA_JOURNAL_HEADER_B);
    if (fault == 1) copies[0] = {99};
    if (fault == 2) copies[present[1] ? 1 : 2][0] ^= 1;
    for (unsigned index = 0; index < copies.size(); ++index) {
      auto entry = std::span(review).subspan(60 + (index + 1) * 68, 68);
      entry[1] = present[index];
      course_review_detail::number(entry, 28, copies[index].size(), 8);
      if (present[index]) SHA256(copies[index].data(), copies[index].size(), entry.data() + 36);
    }
    course_review_detail::number(review, review.size() - 4, binary_record::crc32(review.data(), review.size() - 4), 4);
    Digest expected{};
    ASSERT_TRUE(hash(nullptr, review, expected));
    std::string prefix = "/.crosspoint/companion/course-review-state-";
    static constexpr char DIGITS[] = "0123456789abcdef";
    for (const auto byte : expected) {
      prefix += DIGITS[byte >> 4];
      prefix += DIGITS[byte & 15];
    }
    for (unsigned index = 0; index < copies.size(); ++index)
      if (present[index]) hal.files[prefix + "-0" + DIGITS[index + 1]] = copies[index];
    if (fault == 3) hal.files[prefix + "-01"] = {8};
    const auto retained = hal.files;
    auto snapshot =
        makeUniqueNoThrow<CourseBaselineJournalSnapshot>(storage, [](void*) { return true; }, nullptr, hash, nullptr);
    ASSERT_TRUE(snapshot);
    std::copy(review.begin(), review.end(), scratch.begin());
    const auto opened = snapshot->open(std::span(scratch).first(review.size()), expected, scratch);
    if (fault == 3) {
      EXPECT_EQ(opened, CourseBaselineJournalSnapshotResult::IoError);
    } else {
      ASSERT_EQ(opened, CourseBaselineJournalSnapshotResult::Ok);
      auto journal = makeUniqueNoThrow<TintaJournal>(*snapshot, scratch);
      ASSERT_TRUE(journal);
      EXPECT_EQ(journal->open(), fault == 0   ? TintaJournalResult::Ok
                                 : fault == 1 ? TintaJournalResult::IoError
                                              : TintaJournalResult::Corrupt);
      EXPECT_FALSE(snapshot->write(0, std::span(review).first(1)));
      EXPECT_FALSE(snapshot->truncate(0));
      EXPECT_FALSE(snapshot->writeHeader(0, std::span(review).first(64)));
    }
    EXPECT_EQ(hal.files, retained);
    if (fault == 0) {
      struct Cleanup {
        CourseBaselineJournalSnapshot* snapshot;
        unsigned calls = 0;
        bool succeeds = true;
      } cleanup{snapshot.get()};
      auto close = [](void* context) {
        auto& cleanup = *static_cast<Cleanup*>(context);
        ++cleanup.calls;
        cleanup.snapshot->close();
        return cleanup.succeeds;
      };
      auto audit = makeUniqueNoThrow<HalJournalCausalAuditSession>(*snapshot, close, &cleanup);
      ASSERT_TRUE(audit);
      Digest frontier{};
      ASSERT_TRUE(audit->run(&frontier));
      EXPECT_EQ(audit->recordCount(), 0u);
      EXPECT_NE(frontier, Digest{});
      EXPECT_EQ(cleanup.calls, 1u);
      audit.reset();
      EXPECT_EQ(cleanup.calls, 2u);
      for (const auto& [path, data] : retained) EXPECT_EQ(hal.files.at(path), data);
      ASSERT_EQ(snapshot->reopen(scratch), CourseBaselineJournalSnapshotResult::Ok);
      uint32_t reopenedLength = UINT32_MAX;
      ASSERT_TRUE(snapshot->size(reopenedLength));
      EXPECT_EQ(reopenedLength, 0u);
      snapshot->close();
      EXPECT_EQ(snapshot->reopen({}), CourseBaselineJournalSnapshotResult::Invalid);
      for (const auto& [path, data] : retained) {
        if (!path.starts_with("/.crosspoint/companion/course-review-state-")) continue;
        auto& changed = hal.files.at(path);
        if (changed.empty())
          changed.resize(1, 1);
        else
          changed.back() ^= 1;
        EXPECT_EQ(snapshot->reopen(scratch), CourseBaselineJournalSnapshotResult::IoError);
        EXPECT_FALSE(snapshot->size(reopenedLength));
        changed = data;
      }
      ASSERT_EQ(snapshot->open(review, expected, scratch), CourseBaselineJournalSnapshotResult::Ok);
      cleanup.succeeds = false;
      audit = makeUniqueNoThrow<HalJournalCausalAuditSession>(*snapshot, close, &cleanup);
      ASSERT_TRUE(audit);
      frontier.fill(9);
      const auto untouched = frontier;
      EXPECT_FALSE(audit->run(&frontier));
      EXPECT_EQ(frontier, untouched);
      audit.reset();
      for (const auto& [path, data] : retained) EXPECT_EQ(hal.files.at(path), data);
    }
  }
}

TEST_F(HalCourseTransferTest, ReviewedJournalAuditChecksCandidateSubjectsFromFrozenCopiesAndCleansUp) {
  const auto initial = inventory_hal_test::state;
  for (const bool missing : {false, true}) {
    SCOPED_TRACE(missing);
    inventory_hal_test::state = initial;
    auto& hal = inventory_hal_test::state;
    hal.enumerateFileMap = true;
    ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
    hal.files[ACTIVE_COURSE_PATH] = bytes;
    HalInventoryIndexStorage packStorage;
    ASSERT_TRUE(packStorage.open(ACTIVE_COURSE_PATH));
    StoredCourseSource source(packStorage);
    ASSERT_TRUE(source.attach());
    auto pack = makeUniqueNoThrow<tinta::core::pack::Pack>();
    ASSERT_TRUE(pack);
    ASSERT_EQ(validateCourseCandidate(*pack, source, scratch), CourseValidationResult::Ok);
    TintaPackSubjectCatalog catalog(*pack, source);
    ASSERT_TRUE(catalog.prepare(scratch));
    {
      HalTintaJournalStorage native;
      auto journal = makeUniqueNoThrow<TintaJournal>(native, scratch);
      ASSERT_TRUE(journal);
      ASSERT_EQ(journal->open(), TintaJournalResult::Ok);
      TintaBody body;
      body.kind = EventKind::Star;
      body.course = declaration.manifest.logicalIdentity;
      body.uid = missing ? UINT32_MAX - 1 : pack->uidAt(0);
      body.enabled = true;
      std::array<uint8_t, MAX_TINTA_BODY_SIZE> encoded{};
      const auto length = encodeTintaBody(body, encoded);
      ASSERT_NE(length, 0u);
      SyncEvent event;
      event.identity = {declaration.state.owner, 1, 1};
      event.storageGeneration = generation;
      event.kind = body.kind;
      event.resource = declaration.manifest.contentHash;
      ASSERT_TRUE(native.digest(std::span(encoded).first(length), event.bodyHash));
      ASSERT_EQ(journal->append(event, std::span(encoded).first(length)), TintaJournalResult::Ok);
      ASSERT_TRUE(native.close());
    }
    Digest expected{};
    const auto review = reviewedJournalCopies(expected);
    ASSERT_FALSE(review.empty());
    const auto retained = hal.files;
    auto hash = [](void*, std::span<const uint8_t> input, Digest& output) {
      return SHA256(input.data(), input.size(), output.data()) != nullptr;
    };
    auto snapshot =
        makeUniqueNoThrow<CourseBaselineJournalSnapshot>(storage, [](void*) { return true; }, nullptr, hash, nullptr);
    ASSERT_TRUE(snapshot);
    ASSERT_EQ(snapshot->open(review, expected, scratch), CourseBaselineJournalSnapshotResult::Ok);
    auto audit = makeUniqueNoThrow<HalJournalCausalAuditSession>(
        *snapshot,
        [](void* context) {
          static_cast<CourseBaselineJournalSnapshot*>(context)->close();
          return true;
        },
        snapshot.get());
    ASSERT_TRUE(audit);
    Digest frontier{};
    EXPECT_EQ(audit->run(&frontier, &declaration.manifest.logicalIdentity, &catalog), !missing);
    EXPECT_EQ(frontier == Digest{}, missing);
    audit.reset();
    for (const auto& [path, data] : retained) EXPECT_EQ(hal.files.at(path), data);
    uint32_t length = 17;
    EXPECT_FALSE(snapshot->size(length));
    EXPECT_EQ(length, 17u);
    auto reviewed = createHalCourseBaselineReviewedJournalAudit(*pack, source, [](void*) { return true; }, nullptr);
    ASSERT_TRUE(reviewed);
    EXPECT_EQ(reviewed->journalFrontier(), nullptr);
    struct ReplayCheck {
      Identity course;
      uint32_t uid;
      unsigned calls = 0;
    } check{declaration.manifest.logicalIdentity, pack->uidAt(0)};
    auto visitor = [](void* context, uint32_t, const SyncEvent& event, std::span<const uint8_t> bytes, bool undone) {
      auto& check = *static_cast<ReplayCheck*>(context);
      ++check.calls;
      TintaBody body;
      if (!decodeTintaBody(bytes, body)) return false;
      EXPECT_EQ(event.kind, EventKind::Star);
      EXPECT_EQ(body.course, check.course);
      EXPECT_EQ(body.uid, check.uid);
      EXPECT_TRUE(body.enabled);
      EXPECT_FALSE(undone);
      return event.kind == EventKind::Star && body.course == check.course && body.uid == check.uid && !undone;
    };
    EXPECT_FALSE(reviewed->replay(&check, visitor, scratch));
    EXPECT_EQ(reviewed->run(review, expected, declaration.manifest.logicalIdentity, scratch), !missing);
    EXPECT_EQ(reviewed->journalFrontier(), nullptr);
    EXPECT_EQ(reviewed->replay(&check, visitor, scratch), !missing);
    if (!missing) {
      ASSERT_NE(reviewed->journalFrontier(), nullptr);
      EXPECT_EQ(*reviewed->journalFrontier(), frontier);
    } else {
      EXPECT_EQ(reviewed->journalFrontier(), nullptr);
    }
    EXPECT_EQ(check.calls, missing ? 0u : 1u);
    EXPECT_FALSE(reviewed->replay(&check, visitor, scratch));
    EXPECT_EQ(reviewed->journalFrontier(), nullptr);
    for (const auto& [path, data] : retained) EXPECT_EQ(hal.files.at(path), data);
    if (!missing) {
      auto backup = std::find_if(hal.files.begin(), hal.files.end(), [](const auto& entry) {
        return entry.first.starts_with("/.crosspoint/companion/course-review-state-") && entry.first.ends_with("-01");
      });
      ASSERT_NE(backup, hal.files.end());
      ASSERT_FALSE(backup->second.empty());
      const auto path = backup->first;
      const auto original = backup->second;
      bool permitted = true;
      auto guarded = createHalCourseBaselineReviewedJournalAudit(
          *pack, source, [](void* context) { return *static_cast<bool*>(context); }, &permitted);
      ASSERT_TRUE(guarded);
      struct Fault {
        bool* permitted;
        const std::string* path;
        unsigned mode, calls = 0;
      } fault{&permitted, &path, 0};
      auto reject = [](void* context, uint32_t, const SyncEvent&, std::span<const uint8_t>, bool) {
        auto& fault = *static_cast<Fault*>(context);
        ++fault.calls;
        if (fault.mode == 1) return false;
        if (fault.mode == 2) *fault.permitted = false;
        if (fault.mode == 3) inventory_hal_test::state.files.at(*fault.path).back() ^= 1;
        return true;
      };
      for (unsigned mode = 0; mode < 4; ++mode) {
        SCOPED_TRACE(mode);
        fault.mode = mode;
        fault.calls = 0;
        permitted = true;
        hal.files.at(path) = original;
        ASSERT_TRUE(guarded->run(review, expected, declaration.manifest.logicalIdentity, scratch));
        EXPECT_EQ(guarded->journalFrontier(), nullptr);
        if (mode == 0) hal.files.at(path).back() ^= 1;
        EXPECT_FALSE(guarded->replay(&fault, reject, scratch));
        EXPECT_EQ(guarded->journalFrontier(), nullptr);
        EXPECT_EQ(fault.calls, mode == 0 ? 0u : 1u);
        permitted = true;
        EXPECT_FALSE(guarded->replay(&fault, reject, scratch));
        for (const auto& [name, data] : retained) {
          if (name != path) {
            EXPECT_EQ(hal.files.at(name), data);
          }
        }
      }
      hal.files.at(path) = original;
      ASSERT_TRUE(guarded->run(review, expected, declaration.manifest.logicalIdentity, scratch));
      ASSERT_TRUE(guarded->replay(&check, visitor, scratch));
      ASSERT_NE(guarded->journalFrontier(), nullptr);
      EXPECT_EQ(*guarded->journalFrontier(), frontier);
      permitted = false;
      EXPECT_EQ(guarded->journalFrontier(), nullptr);
      EXPECT_FALSE(guarded->run(review, expected, declaration.manifest.logicalIdentity, scratch));
      permitted = true;
      EXPECT_EQ(guarded->journalFrontier(), nullptr);
    }
    bool projectionAllowed = true;
    auto projection = createHalCourseBaselineReplaySession(
        *pack, source, [](void* context) { return *static_cast<bool*>(context); }, &projectionAllowed);
    ASSERT_TRUE(projection);
    EXPECT_EQ(projection->workingStore(), nullptr);
    EXPECT_EQ(projection->journalFrontier(), nullptr);
    EXPECT_EQ(projection->run(review, expected, declaration.manifest.logicalIdentity, scratch), !missing);
    if (!missing) {
      ASSERT_NE(projection->workingStore(), nullptr);
      ASSERT_NE(projection->journalFrontier(), nullptr);
      EXPECT_EQ(*projection->journalFrontier(), frontier);
      tinta::core::ItemState projected;
      ASSERT_TRUE(projection->workingStore()->item(pack->uidAt(0), projected));
      EXPECT_TRUE(projected.flags & tinta::core::item_flag::kStarred);
      EXPECT_EQ(projected.reps, 0);
      auto& projectedStore = *projection->workingStore();
      auto itemExport = makeUniqueNoThrow<HalTintaReplayItemExport>(TintaReplayExportTarget::BaselineProof);
      ASSERT_TRUE(itemExport);
      ASSERT_TRUE(itemExport->run(*projection->workingStore(), declaration.manifest.logicalIdentity, 0));
      ASSERT_NE(itemExport->verifiedPath(), nullptr);
      const std::string itemPath = itemExport->verifiedPath();
      const auto itemBytes = hal.files.at(itemPath);
      HalFile itemFile;
      ASSERT_TRUE(Storage.openFileForRead("TEST", itemPath.c_str(), itemFile));
      CourseUidLookup itemCatalog(source);
      ASSERT_TRUE(itemCatalog.begin());
      auto permission = [](void* context) { return *static_cast<bool*>(context); };
      ASSERT_TRUE(compareTintaReplayItems(itemFile, projectedStore, declaration.manifest.logicalIdentity, itemCatalog,
                                          scratch, permission, &projectionAllowed));
      EXPECT_EQ(hal.files.at(itemPath), itemBytes);
      auto& changedItems = hal.files.at(itemPath);
      auto changed = projected;
      changed.flags = 0;
      changed.encode(changedItems.data() + 1024);
      EXPECT_FALSE(compareTintaReplayItems(itemFile, projectedStore, declaration.manifest.logicalIdentity, itemCatalog,
                                           scratch, permission, &projectionAllowed));
      for (const size_t at : {size_t(0), size_t(512)}) {
        binary_record::putU16(changedItems.data() + at + 26, 1);
        binary_record::putU32(changedItems.data() + at + 28, 0);
        projected.encode(changedItems.data() + at + 32);
        binary_record::putU32(changedItems.data() + at + 76, binary_record::crc32(changedItems.data() + at, 76));
      }
      ASSERT_TRUE(compareTintaReplayItems(itemFile, projectedStore, declaration.manifest.logicalIdentity, itemCatalog,
                                          scratch, permission, &projectionAllowed));
      changedItems = itemBytes;
      changedItems.resize(1024);
      for (const size_t at : {size_t(0), size_t(512)}) {
        binary_record::putU32(changedItems.data() + at + 12, 0);
        binary_record::putU32(changedItems.data() + at + 76, binary_record::crc32(changedItems.data() + at, 76));
      }
      EXPECT_FALSE(compareTintaReplayItems(itemFile, projectedStore, declaration.manifest.logicalIdentity, itemCatalog,
                                           scratch, permission, &projectionAllowed));
      changedItems = itemBytes;
      for (const size_t at : {size_t(0), size_t(512)}) {
        binary_record::putU16(changedItems.data() + at + 22, 1);
        binary_record::putU32(changedItems.data() + at + 76, binary_record::crc32(changedItems.data() + at, 76));
      }
      EXPECT_FALSE(compareTintaReplayItems(itemFile, projectedStore, declaration.manifest.logicalIdentity, itemCatalog,
                                           scratch, permission, &projectionAllowed));
      changedItems = itemBytes;
      EXPECT_FALSE(compareTintaReplayItems(itemFile, projectedStore, declaration.manifest.logicalIdentity, itemCatalog,
                                           std::span(scratch).first(159), permission, &projectionAllowed));
      auto receiptExport = makeUniqueNoThrow<HalTintaReplayExport>(TintaReplayExportTarget::BaselineProof);
      ASSERT_TRUE(receiptExport);
      ASSERT_TRUE(receiptExport->run(projectedStore, declaration.manifest.logicalIdentity, 0, scratch));
      Identity snapshot{1};
      ASSERT_TRUE(
          receiptExport->manifest(generation, declaration.manifest.contentHash, frontier, snapshot, 1, scratch));
      std::array<uint8_t, TINTA_DERIVED_MANIFEST_SIZE> receipt{};
      std::copy_n(scratch.begin(), receipt.size(), receipt.begin());
      Digest receiptHash{};
      ASSERT_NE(SHA256(receipt.data(), receipt.size(), receiptHash.data()), nullptr);
      auto receiptProof = createHalCourseBaselineReplayReceipt(permission, &projectionAllowed);
      ASSERT_TRUE(receiptProof);
      EXPECT_EQ(receiptProof->sessionSnapshot(), nullptr);
      ASSERT_TRUE(receiptProof->run(*projection, std::span(scratch).first(receipt.size()), receiptHash,
                                    declaration.manifest.logicalIdentity, generation, declaration.manifest.contentHash,
                                    scratch));
      ASSERT_NE(receiptProof->sessionSnapshot(), nullptr);
      EXPECT_EQ(*receiptProof->sessionSnapshot(), receiptHash);
      EXPECT_NE(*receiptProof->sessionSnapshot(), frontier);
      auto damagedHash = receiptHash;
      damagedHash[0] ^= 1;
      EXPECT_FALSE(receiptProof->run(*projection, receipt, damagedHash, declaration.manifest.logicalIdentity,
                                     generation, declaration.manifest.contentHash, scratch));
      EXPECT_EQ(receiptProof->sessionSnapshot(), nullptr);
      receipt[136] ^= 1;
      binary_record::putU32(receipt.data() + 328, binary_record::crc32(receipt.data(), 328));
      ASSERT_NE(SHA256(receipt.data(), receipt.size(), damagedHash.data()), nullptr);
      EXPECT_FALSE(receiptProof->run(*projection, receipt, damagedHash, declaration.manifest.logicalIdentity,
                                     generation, declaration.manifest.contentHash, scratch));
      EXPECT_EQ(receiptProof->sessionSnapshot(), nullptr);
      projectionAllowed = false;
      EXPECT_FALSE(compareTintaReplayItems(itemFile, projectedStore, declaration.manifest.logicalIdentity, itemCatalog,
                                           scratch, permission, &projectionAllowed));
      EXPECT_EQ(projection->workingStore(), nullptr);
      EXPECT_EQ(projection->journalFrontier(), nullptr);
      EXPECT_FALSE(projection->run(review, expected, declaration.manifest.logicalIdentity, scratch));
      projectionAllowed = true;
    }
    EXPECT_EQ(projection->workingStore(), nullptr);
    EXPECT_EQ(projection->journalFrontier(), nullptr);
    EXPECT_FALSE(projection->run(review, expected, declaration.manifest.logicalIdentity,
                                 {reinterpret_cast<uint8_t*>(projection.get()), sizeof(*projection)}));
    EXPECT_EQ(projection->workingStore(), nullptr);
    EXPECT_EQ(projection->journalFrontier(), nullptr);
    for (const auto& [path, data] : retained) EXPECT_EQ(hal.files.at(path), data);
    for (const auto& [path, data] : hal.files) {
      if (!retained.contains(path)) {
        EXPECT_FALSE(path.starts_with(COURSE_STATE_ROOT));
      }
    }
    ASSERT_TRUE(packStorage.close());
  }
}

TEST_F(HalCourseTransferTest, ReviewedJournalAbsenceCannotCreateAuthorityAndRequiresExactReviewAndPermission) {
  ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
  std::ifstream fixture(std::string(COMPANION_FIXTURE_DIR) + "CourseBaselineReview-v1.fixture", std::ios::binary);
  std::vector<uint8_t> review{std::istreambuf_iterator<char>(fixture), std::istreambuf_iterator<char>()};
  ASSERT_EQ(review.size(), 608u);
  auto hash = [](void*, std::span<const uint8_t> input, Digest& output) {
    return SHA256(input.data(), input.size(), output.data()) != nullptr;
  };
  Digest expected{};
  ASSERT_TRUE(hash(nullptr, review, expected));
  bool allowed = true;
  auto snapshot = makeUniqueNoThrow<CourseBaselineJournalSnapshot>(
      storage, [](void* context) { return *static_cast<bool*>(context); }, &allowed, hash, nullptr);
  ASSERT_TRUE(snapshot);
  const auto retained = inventory_hal_test::state.files;
  auto wrongHash = expected;
  wrongHash[0] ^= 1;
  EXPECT_EQ(snapshot->open(review, wrongHash, scratch), CourseBaselineJournalSnapshotResult::Invalid);
  allowed = false;
  EXPECT_EQ(snapshot->open(review, expected, scratch), CourseBaselineJournalSnapshotResult::Busy);
  allowed = true;
  EXPECT_EQ(snapshot->open(review, expected, scratch), CourseBaselineJournalSnapshotResult::Missing);
  auto journal = makeUniqueNoThrow<TintaJournal>(*snapshot, scratch);
  ASSERT_TRUE(journal);
  EXPECT_EQ(journal->open(), TintaJournalResult::IoError);
  EXPECT_FALSE(journal->available());
  EXPECT_EQ(inventory_hal_test::state.files, retained);
}

TEST_F(HalCourseTransferTest, NativeOrphanConsentRecoveryPreservesBytesWithoutGrantingApproval) {
  CourseBaselineImportRequest request;
  Identity reader{};
  std::string path;
  prepareBaselineApproval(request, reader, path);
  ASSERT_FALSE(HasFatalFailure());
  auto& hal = inventory_hal_test::state;
  hal.files[path + ".tmp"] = {1, 2, 3};
  auto other = path;
  other[std::string_view("/.crosspoint/companion/course-baseline-").size()] = '1';
  hal.files[other + ".tmp"] = {};
  const auto evidence = hal.files;
  auto permitted = [](void*) { return true; };
  bool pending = false;
  ASSERT_TRUE(hasHalCourseBaselineOrphanConsents(pending, permitted, nullptr));
  ASSERT_TRUE(pending);
  Transfer parent(storage, scratch);
  ASSERT_TRUE(recoverHalCourseBaselineOrphanConsents(parent, generation, permitted, nullptr));
  EXPECT_EQ(parent.current(), nullptr);
  EXPECT_FALSE(hal.files.contains(path));
  EXPECT_FALSE(hal.files.contains(other));
  EXPECT_FALSE(hal.files.contains(path + ".tmp"));
  EXPECT_FALSE(hal.files.contains(other + ".tmp"));
  EXPECT_EQ(hal.files.at(path + ".orphan"), evidence.at(path + ".tmp"));
  EXPECT_EQ(hal.files.at(other + ".orphan"), evidence.at(other + ".tmp"));
  for (const auto& [name, bytes] : evidence) {
    if (name != path + ".tmp" && name != other + ".tmp") {
      EXPECT_EQ(hal.files.at(name), bytes);
    }
  }
  const auto complete = hal.files;
  ASSERT_TRUE(hasHalCourseBaselineOrphanConsents(pending, permitted, nullptr));
  EXPECT_FALSE(pending);
  EXPECT_TRUE(recoverHalCourseBaselineOrphanConsents(parent, generation, permitted, nullptr));
  EXPECT_EQ(hal.files, complete);
  hal.files[path + ".tmp"] = {4, 5};
  ASSERT_TRUE(recoverHalCourseBaselineOrphanConsents(parent, generation, permitted, nullptr));
  EXPECT_EQ(hal.files.at(path + ".orphan"), evidence.at(path + ".tmp"));
  EXPECT_EQ(hal.files.at(path + ".orphan-00"), (std::vector<uint8_t>{4, 5}));
  EXPECT_FALSE(hal.files.contains(path));
  EXPECT_FALSE(hal.files.contains(path + ".tmp"));
}

TEST_F(HalCourseTransferTest, NativeOrphanConsentRecoveryRefusesParentAuthorityAndNamespaceAmbiguity) {
  for (unsigned fault = 0; fault < 10; ++fault) {
    SetUp();
    SCOPED_TRACE(fault);
    CourseBaselineImportRequest request;
    Identity reader{};
    std::string path;
    prepareBaselineApproval(request, reader, path);
    ASSERT_FALSE(HasFatalFailure());
    auto& hal = inventory_hal_test::state;
    Transfer parent(storage, scratch);
    if (fault == 5) {
      ASSERT_EQ(parent.recover(generation), TransferResult::Ok);
      ASSERT_EQ(parent.begin(declaration, COURSE_BASELINE_DESTINATION), TransferResult::Ok);
    }
    hal.files[path + ".tmp"] = {1, 2, 3};
    const auto prefix = path.substr(0, path.size() - std::string_view(".consent").size());
    static constexpr const char* PROTECTED[] = {".consent", ".prepared", ".prepared.tmp", ".published",
                                                ".published.tmp"};
    if (fault < 5) hal.files[prefix + PROTECTED[fault]] = {9};
    if (fault == 6) hal.files[TRANSFER_JOURNALS[0]] = {1};
    if (fault == 7) {
      auto duplicate = path + ".tmp";
      duplicate[std::string_view(TRANSFER_DIRECTORY).size() + 1] = 'C';
      hal.files[duplicate] = {8};
    }
    if (fault == 8) hal.failShortName = true;
    bool permitted = fault != 9;
    const auto evidence = hal.files;
    EXPECT_FALSE(recoverHalCourseBaselineOrphanConsents(
        parent, generation, [](void* raw) { return *static_cast<bool*>(raw); }, &permitted));
    EXPECT_EQ(hal.files, evidence);
    EXPECT_FALSE(hal.files.contains(path + ".orphan"));
  }
}

TEST_F(HalCourseTransferTest, NativeOrphanConsentRecoveryRetriesRenamePowerCutsWithEvidenceIntact) {
  for (const bool after : {false, true}) {
    SetUp();
    CourseBaselineImportRequest request;
    Identity reader{};
    std::string path;
    prepareBaselineApproval(request, reader, path);
    ASSERT_FALSE(HasFatalFailure());
    auto& hal = inventory_hal_test::state;
    const std::vector<uint8_t> bytes{1, 2, 3};
    hal.files[path + ".tmp"] = bytes;
    if (after)
      hal.failRenameAfter = hal.renames + 1;
    else
      hal.failRename = hal.renames + 1;
    Transfer parent(storage, scratch);
    auto permitted = [](void*) { return true; };
    EXPECT_FALSE(recoverHalCourseBaselineOrphanConsents(parent, generation, permitted, nullptr));
    EXPECT_EQ(hal.files.at(path + (after ? ".orphan" : ".tmp")), bytes);
    EXPECT_FALSE(hal.files.contains(path));
    hal.failRename = hal.failRenameAfter = 0;
    EXPECT_TRUE(recoverHalCourseBaselineOrphanConsents(parent, generation, permitted, nullptr));
    EXPECT_EQ(hal.files.at(path + ".orphan"), bytes);
    EXPECT_FALSE(hal.files.contains(path + ".tmp"));
  }
}

TEST_F(HalCourseTransferTest, BaselineRecoveryRequiresNativeIdentityKnownOwnerAndExclusiveWorkspace) {
  CourseBaselineImportRequest request;
  Identity reader{};
  std::string consentPath;
  prepareBaselineApproval(request, reader, consentPath);
  ASSERT_FALSE(HasFatalFailure());
  Transfer transfer(storage, scratch);
  ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
  ASSERT_EQ(transfer.begin(declaration, COURSE_BASELINE_DESTINATION), TransferResult::Ok);
  BaselineRecoveryIdentities identities(reader, generation);
  BaselineRecoveryPairings pairingStorage;
  Pairings pairings(pairingStorage);
  IdentityState identity{reader, generation, 1};
  bool exclusive = true;
  auto permitted = [](void* context) { return *static_cast<bool*>(context); };
  std::unique_ptr<HalCourseBaselineImportSession> session;
  const auto files = inventory_hal_test::state.files;
  EXPECT_FALSE(attachHalCourseBaselineRecovery(transfer, storage, identities, identity, pairings, scratch, scratch,
                                               session, permitted, &exclusive));
  EXPECT_FALSE(session);
  ASSERT_EQ(pairings.load(scratch), PairingResult::Ok);
  PairingSecret secret{};
  secret[0] = 12;
  PairingPeer peer{};
  peer[0] = 13;
  ASSERT_EQ(pairings.add(request.owner, secret, peer, scratch), PairingResult::Ok);
  for (unsigned fault = 0; fault < 4; ++fault) {
    SCOPED_TRACE(fault);
    identities.fail = fault == 0;
    pairingStorage.fail = fault == 1;
    exclusive = fault != 2;
    auto supplied = identity;
    if (fault == 3) supplied.device[0] ^= 0x80;
    EXPECT_FALSE(attachHalCourseBaselineRecovery(transfer, storage, identities, supplied, pairings, scratch, scratch,
                                                 session, permitted, &exclusive));
    EXPECT_FALSE(session);
    EXPECT_EQ(inventory_hal_test::state.files, files);
    EXPECT_EQ(identities.writes, 0U);
  }
  identities.fail = pairingStorage.fail = false;
  exclusive = true;
  EXPECT_TRUE(attachHalCourseBaselineRecovery(transfer, storage, identities, identity, pairings, scratch, scratch,
                                              session, permitted, &exclusive));
  ASSERT_TRUE(session);
  EXPECT_EQ(transfer.commit(request.transaction, request.owner), TransferResult::NoTransaction);
  EXPECT_EQ(inventory_hal_test::state.files, files);
  EXPECT_EQ(identities.writes, 0U);
  storage.setCourseBaselineInstaller(nullptr);
}

TEST_F(HalCourseTransferTest, NativeBaselineBeginPreservesReviewedFilesAndRetriesExactConsent) {
  CourseBaselineImportRequest request;
  Identity reader{};
  std::string consentPath;
  prepareBaselineApproval(request, reader, consentPath);
  ASSERT_FALSE(HasFatalFailure());
  Transfer transfer(storage, std::span(scratch).subspan(TRANSFER_OFFSET));
  ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
  const auto original = inventory_hal_test::state.files;
  bool allowed = true;
  auto permitted = [](void* context) { return *static_cast<bool*>(context); };
  ASSERT_EQ(
      beginHalCourseBaselineImport(transfer, reader, generation, request.owner, request, scratch, permitted, &allowed),
      TransferResult::Ok);
  ASSERT_NE(transfer.current(), nullptr);
  EXPECT_EQ(transfer.destination(), COURSE_BASELINE_DESTINATION);
  EXPECT_EQ(transfer.current()->durableOffset, 0u);
  EXPECT_EQ(transfer.current()->transaction, request.transaction);
  EXPECT_TRUE(inventory_hal_test::state.files.contains(consentPath));
  for (const auto& [path, bytes] : original) EXPECT_EQ(inventory_hal_test::state.files.at(path), bytes);
  const auto approved = inventory_hal_test::state.files;
  ASSERT_EQ(
      beginHalCourseBaselineImport(transfer, reader, generation, request.owner, request, scratch, permitted, &allowed),
      TransferResult::Ok);
  EXPECT_EQ(inventory_hal_test::state.files, approved);
  allowed = false;
  EXPECT_EQ(
      beginHalCourseBaselineImport(transfer, reader, generation, request.owner, request, scratch, permitted, &allowed),
      TransferResult::Unauthorized);
  EXPECT_EQ(inventory_hal_test::state.files, approved);
}

TEST_F(HalCourseTransferTest, NativeBaselineBeginRefusesWrongContextAndHeapBeforeConsentOrUpload) {
  CourseBaselineImportRequest request;
  Identity reader{};
  std::string consentPath;
  prepareBaselineApproval(request, reader, consentPath);
  ASSERT_FALSE(HasFatalFailure());
  Transfer transfer(storage, std::span(scratch).subspan(TRANSFER_OFFSET));
  ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
  const auto original = inventory_hal_test::state.files;
  auto permitted = [](void*) { return true; };
  Identity wrong{};
  wrong.fill(9);
  EXPECT_EQ(beginHalCourseBaselineImport(transfer, reader, generation, wrong, request, scratch, permitted, nullptr),
            TransferResult::Unauthorized);
  EXPECT_EQ(beginHalCourseBaselineImport(transfer, reader, wrong, request.owner, request, scratch, permitted, nullptr),
            TransferResult::WrongStorage);
  EXPECT_EQ(beginHalCourseBaselineImport(transfer, reader, generation, request.owner, request,
                                         std::span(scratch).first(scratch.size() - 1), permitted, nullptr),
            TransferResult::Invalid);
  companion_memory_test::internal.freeBytes = 50 * 1024;
  EXPECT_EQ(
      beginHalCourseBaselineImport(transfer, reader, generation, request.owner, request, scratch, permitted, nullptr),
      TransferResult::Unauthorized);
  EXPECT_EQ(inventory_hal_test::state.files, original);
  EXPECT_EQ(transfer.current(), nullptr);
  companion_memory_test::internal = {1024 * 1024, 1024 * 1024, 1024 * 1024, 1024 * 1024};
  inventory_hal_test::state.directories[TINTA_JOURNAL_DIRECTORY] = {};
  inventory_hal_test::state.files[TINTA_JOURNAL_EVENTS] = {1};
  const auto malformedJournal = inventory_hal_test::state.files;
  EXPECT_EQ(
      beginHalCourseBaselineImport(transfer, reader, generation, request.owner, request, scratch, permitted, nullptr),
      TransferResult::Unauthorized);
  EXPECT_EQ(inventory_hal_test::state.files, malformedJournal);
  EXPECT_FALSE(inventory_hal_test::state.files.contains(consentPath));
  EXPECT_EQ(transfer.current(), nullptr);
}

TEST_F(HalCourseTransferTest, NativeBaselineCommitRefusesContextAndHeapBeforeInstallation) {
  CourseBaselineImportRequest request;
  Identity reader{};
  std::string consentPath;
  prepareBaselineApproval(request, reader, consentPath);
  ASSERT_FALSE(HasFatalFailure());
  auto parentWorkspace = std::span(scratch).subspan(TRANSFER_OFFSET);
  Transfer transfer(storage, parentWorkspace);
  ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
  auto permitted = [](void*) { return true; };
  ASSERT_EQ(
      beginHalCourseBaselineImport(transfer, reader, generation, request.owner, request, scratch, permitted, nullptr),
      TransferResult::Ok);
  const auto approved = inventory_hal_test::state.files;
  auto existingSession = createHalCourseBaselineImportSession(reader, generation, request.owner, scratch, {8, 40, 200},
                                                              permitted, nullptr, parentWorkspace);
  ASSERT_TRUE(existingSession);
  storage.setCourseBaselineInstaller(existingSession->installer());
  EXPECT_EQ(commitHalCourseBaselineImport(transfer, storage, reader, generation, request.owner, request.transaction,
                                          scratch, parentWorkspace, {8, 40, 200}, permitted, nullptr),
            TransferResult::Busy);
  EXPECT_TRUE(storage.hasCourseBaselineInstaller());
  EXPECT_EQ(inventory_hal_test::state.files, approved);
  storage.setCourseBaselineInstaller(nullptr);
  existingSession.reset();

  Identity wrong{};
  wrong.fill(9);
  EXPECT_EQ(commitHalCourseBaselineImport(transfer, storage, reader, generation, wrong, request.transaction, scratch,
                                          parentWorkspace, {8, 40, 200}, permitted, nullptr),
            TransferResult::Unauthorized);
  EXPECT_EQ(commitHalCourseBaselineImport(transfer, storage, reader, wrong, request.owner, request.transaction, scratch,
                                          parentWorkspace, {8, 40, 200}, permitted, nullptr),
            TransferResult::WrongStorage);
  EXPECT_EQ(commitHalCourseBaselineImport(transfer, storage, reader, generation, request.owner, wrong, scratch,
                                          parentWorkspace, {8, 40, 200}, permitted, nullptr),
            TransferResult::Invalid);
  EXPECT_EQ(commitHalCourseBaselineImport(transfer, storage, reader, generation, request.owner, request.transaction,
                                          scratch, parentWorkspace, {8, 40, 200}, permitted, nullptr),
            TransferResult::Offset);
  EXPECT_EQ(inventory_hal_test::state.files, approved);
  for (size_t at = 0; at < bytes.size();) {
    const auto count = std::min<size_t>(1000, bytes.size() - at);
    ASSERT_EQ(transfer.append(request.transaction, request.owner, at, std::span(bytes).subspan(at, count)),
              TransferResult::Ok);
    at += count;
  }
  const auto uploaded = inventory_hal_test::state.files;
  EXPECT_EQ(commitHalCourseBaselineImport(transfer, storage, reader, generation, request.owner, request.transaction,
                                          scratch, parentWorkspace.first(parentWorkspace.size() - 1), {8, 40, 200},
                                          permitted, nullptr),
            TransferResult::Invalid);
  companion_memory_test::internal.freeBytes = 50 * 1024;
  EXPECT_EQ(commitHalCourseBaselineImport(transfer, storage, reader, generation, request.owner, request.transaction,
                                          scratch, parentWorkspace, {8, 40, 200}, permitted, nullptr),
            TransferResult::IoError);
  EXPECT_EQ(inventory_hal_test::state.files, uploaded);
  ASSERT_NE(transfer.current(), nullptr);
  EXPECT_EQ(transfer.current()->phase, TransferPhase::Receiving);
}

TEST_F(HalCourseTransferTest, UnboundIntentNativeStorageBindsIdentitiesAndClosesEachPhase) {
  Identity reader{};
  reader.fill(17);
  UnboundCourseMigrationIntent intent;
  intent.reader = reader;
  intent.request.original = {generation, declaration.state.owner, declaration.state.transaction, declaration.manifest,
                             declaration.manifest.contentHash};
  intent.activePack = declaration.manifest;
  bool permitted = true;
  auto permission = [](void* raw) { return *static_cast<bool*>(raw); };
  std::array<uint8_t, SESSION_WORKSPACE_SIZE> workspace{};
  HalUnboundCourseMigrationIntentStore store(reader, generation, workspace, permission, &permitted);
  inventory_hal_test::state.directories[TRANSFER_DIRECTORY] = {};
  inventory_hal_test::state.directories["/tinta"] = {};
  inventory_hal_test::state.files[ACTIVE_COURSE_PATH] = bytes;
  UnboundCourseMigrationIntent loaded;
  EXPECT_EQ(store.load(loaded), UnboundCourseIntentResult::Missing);
  for (unsigned phase = 1; phase <= 3; ++phase) {
    intent.phase = static_cast<UnboundCourseMigrationPhase>(phase);
    ASSERT_EQ(store.persist(intent, declaration.state.owner), UnboundCourseIntentResult::Ok);
    ASSERT_TRUE(store.closeReaders());
    ASSERT_EQ(store.load(loaded), UnboundCourseIntentResult::Ok);
    EXPECT_EQ(loaded, intent);
  }
  const auto files = inventory_hal_test::state.files;
  const auto renames = inventory_hal_test::state.renames;
  ASSERT_EQ(store.persist(intent, declaration.state.owner), UnboundCourseIntentResult::Ok);
  EXPECT_EQ(inventory_hal_test::state.files, files);
  EXPECT_EQ(inventory_hal_test::state.renames, renames);
  EXPECT_EQ(inventory_hal_test::state.files.at(ACTIVE_COURSE_PATH), bytes);
  auto foreign = reader;
  foreign[0] ^= 1;
  HalUnboundCourseMigrationIntentStore other(foreign, generation, workspace, permission, &permitted);
  loaded = {};
  EXPECT_EQ(other.load(loaded), UnboundCourseIntentResult::Conflict);
  EXPECT_EQ(loaded, UnboundCourseMigrationIntent{});
  EXPECT_EQ(other.persist(intent, declaration.state.owner), UnboundCourseIntentResult::Invalid);
  auto foreignGeneration = generation;
  foreignGeneration[0] ^= 2;
  HalUnboundCourseMigrationIntentStore otherCard(reader, foreignGeneration, workspace, permission, &permitted);
  EXPECT_EQ(otherCard.load(loaded), UnboundCourseIntentResult::Conflict);
  EXPECT_EQ(otherCard.persist(intent, declaration.state.owner), UnboundCourseIntentResult::Invalid);
  EXPECT_EQ(loaded, UnboundCourseMigrationIntent{});
  auto foreignOwner = declaration.state.owner;
  foreignOwner[0] ^= 1;
  EXPECT_EQ(store.persist(intent, foreignOwner), UnboundCourseIntentResult::Invalid);
  permitted = false;
  EXPECT_EQ(store.load(loaded), UnboundCourseIntentResult::Busy);
  EXPECT_EQ(inventory_hal_test::state.files, files);
}

TEST_F(HalCourseTransferTest, UnboundIntentNativeStorageRecoversSyncCloseAndRenameCuts) {
  for (unsigned fault = 0; fault < 4; ++fault) {
    inventory_hal_test::state = {};
    inventory_hal_test::state.enumerateFileMap = true;
    inventory_hal_test::state.directories[TRANSFER_DIRECTORY] = {};
    Identity reader{};
    reader.fill(17);
    UnboundCourseMigrationIntent intent;
    intent.reader = reader;
    intent.request.original = {generation, declaration.state.owner, declaration.state.transaction, declaration.manifest,
                               declaration.manifest.contentHash};
    intent.activePack = declaration.manifest;
    auto permission = [](void*) { return true; };
    std::array<uint8_t, SESSION_WORKSPACE_SIZE> workspace{};
    const auto* staged = UnboundCourseMigrationIntentStore::STAGES[0];
    if (fault == 0) inventory_hal_test::state.failSyncPath = staged;
    if (fault == 1) inventory_hal_test::state.failClosePath = staged;
    if (fault == 2) inventory_hal_test::state.failRename = 1;
    if (fault == 3) inventory_hal_test::state.failRenameAfter = 1;
    {
      HalUnboundCourseMigrationIntentStore store(reader, generation, workspace, permission, nullptr);
      EXPECT_EQ(store.persist(intent, declaration.state.owner), UnboundCourseIntentResult::IoError);
    }
    inventory_hal_test::state.failSyncPath.clear();
    inventory_hal_test::state.failClosePath.clear();
    inventory_hal_test::state.failRename = 0;
    inventory_hal_test::state.failRenameAfter = 0;
    HalUnboundCourseMigrationIntentStore restored(reader, generation, workspace, permission, nullptr);
    ASSERT_EQ(restored.persist(intent, declaration.state.owner), UnboundCourseIntentResult::Ok);
    UnboundCourseMigrationIntent loaded;
    ASSERT_EQ(restored.load(loaded), UnboundCourseIntentResult::Ok);
    EXPECT_EQ(loaded, intent);
    EXPECT_FALSE(inventory_hal_test::state.files.contains(staged));
  }
}

TEST_F(HalCourseTransferTest, UnboundPackPairVerificationChecksBytesAndLeavesGlobalStateUntouched) {
  auto& hal = inventory_hal_test::state;
  hal.directories["/tinta"] = {};
  hal.directories[TRANSFER_DIRECTORY] = {};
  const std::string originalPath = std::string(TRANSFER_DIRECTORY) + "/unbound-original.part";
  hal.files[originalPath] = bytes;
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  hal.files["/tinta/items.bin"] = {17};
  UnboundCourseMigrationIntent intent;
  intent.reader.fill(17);
  intent.request.original = {generation, declaration.state.owner, declaration.state.transaction, declaration.manifest,
                             declaration.manifest.contentHash};
  intent.activePack = declaration.manifest;
  struct PermissionContext {
    bool permitted = true;
    bool checking = false;
    bool closeNext = false;
    unsigned probes = 0;
    HalUnboundCoursePackVerification* verifier = nullptr;
    const UnboundCourseMigrationIntent* intent = nullptr;
    const std::string* path = nullptr;
  } permissionContext;
  auto permission = [](void* raw) {
    auto& state = *static_cast<PermissionContext*>(raw);
    if (state.checking && state.verifier) {
      ++state.probes;
      EXPECT_FALSE(state.verifier->verified(*state.intent));
      EXPECT_FALSE(state.verifier->verify(*state.intent, *state.path));
    }
    if (state.closeNext && state.verifier) {
      state.closeNext = false;
      state.verifier->closeReaders();
    }
    return state.permitted;
  };
  auto parser = makeUniqueNoThrow<tinta::core::pack::Pack>();
  ASSERT_TRUE(parser);
  auto verifier = makeUniqueNoThrow<HalUnboundCoursePackVerification>(scratch, *parser, permission, &permissionContext);
  ASSERT_TRUE(verifier);
  permissionContext.verifier = verifier.get();
  permissionContext.intent = &intent;
  permissionContext.path = &originalPath;
  permissionContext.checking = true;
  const auto files = hal.files;
  ASSERT_TRUE(verifier->verify(intent, originalPath));
  EXPECT_TRUE(verifier->verified(intent));
  EXPECT_GT(permissionContext.probes, 0u);
  auto foreign = intent;
  foreign.request.original.transaction[0] ^= 1;
  EXPECT_FALSE(verifier->verified(foreign));
  EXPECT_TRUE(verifier->verified(intent));
  permissionContext.closeNext = true;
  EXPECT_FALSE(verifier->verified(intent));
  EXPECT_FALSE(verifier->verified(intent));
  ASSERT_TRUE(verifier->verify(intent, originalPath));
  permissionContext.closeNext = true;
  EXPECT_FALSE(verifier->verify(intent, originalPath));
  EXPECT_FALSE(verifier->verified(intent));
  ASSERT_TRUE(verifier->verify(intent, originalPath));
  EXPECT_FALSE(parser->isOpen());
  EXPECT_EQ(hal.files, files);
  auto corrupt = intent;
  corrupt.activePack.contentHash[0] ^= 1;
  EXPECT_FALSE(verifier->verify(corrupt, originalPath));
  EXPECT_FALSE(verifier->verified(intent));
  EXPECT_EQ(hal.files, files);
  permissionContext.permitted = false;
  EXPECT_FALSE(verifier->verify(intent, originalPath));
  EXPECT_FALSE(verifier->verified(intent));
  EXPECT_EQ(hal.files, files);
}

TEST_F(HalCourseTransferTest, UnboundPackPairVerificationRejectsMalformedOriginalEvenWithMatchingHash) {
  auto& hal = inventory_hal_test::state;
  hal.directories["/tinta"] = {};
  hal.directories[TRANSFER_DIRECTORY] = {};
  const std::string originalPath = std::string(TRANSFER_DIRECTORY) + "/unbound-original.part";
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  auto damaged = bytes;
  damaged[0] ^= 1;
  hal.files[originalPath] = damaged;
  UnboundCourseMigrationIntent intent;
  intent.reader.fill(17);
  intent.request.original = {generation, declaration.state.owner, declaration.state.transaction, declaration.manifest,
                             declaration.manifest.contentHash};
  SHA256(damaged.data(), damaged.size(), intent.request.original.manifest.contentHash.data());
  intent.activePack = declaration.manifest;
  auto parser = makeUniqueNoThrow<tinta::core::pack::Pack>();
  ASSERT_TRUE(parser);
  auto verifier =
      makeUniqueNoThrow<HalUnboundCoursePackVerification>(scratch, *parser, [](void*) { return true; }, nullptr);
  ASSERT_TRUE(verifier);
  const auto files = hal.files;
  EXPECT_FALSE(verifier->verify(intent, originalPath));
  EXPECT_FALSE(verifier->verified(intent));
  EXPECT_FALSE(parser->isOpen());
  EXPECT_EQ(hal.files, files);
}

TEST_F(HalCourseTransferTest, UnboundPackPairAcceptsCompatibleUpdatedBytesAndRejectsAnotherLocale) {
  auto& hal = inventory_hal_test::state;
  hal.directories["/tinta"] = {};
  hal.directories[TRANSFER_DIRECTORY] = {};
  const std::string originalPath = std::string(TRANSFER_DIRECTORY) + "/unbound-original.part";
  hal.files[originalPath] = bytes;
  UnboundCourseMigrationIntent intent;
  intent.reader.fill(17);
  intent.request.original = {generation, declaration.state.owner, declaration.state.transaction, declaration.manifest,
                             declaration.manifest.contentHash};
  addIdentityHistory();
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  intent.activePack = declaration.manifest;
  ASSERT_NE(intent.activePack.contentHash, intent.request.original.manifest.contentHash);
  auto parser = makeUniqueNoThrow<tinta::core::pack::Pack>();
  ASSERT_TRUE(parser);
  auto verifier =
      makeUniqueNoThrow<HalUnboundCoursePackVerification>(scratch, *parser, [](void*) { return true; }, nullptr);
  ASSERT_TRUE(verifier);
  const auto files = hal.files;
  ASSERT_TRUE(verifier->verify(intent, originalPath));
  EXPECT_EQ(hal.files, files);
  std::fill_n(bytes.begin() + offsetof(tinta::core::pack::Header, locale), 8, 0);
  bytes[offsetof(tinta::core::pack::Header, locale)] = 'f';
  bytes[offsetof(tinta::core::pack::Header, locale) + 1] = 'r';
  sealPack();
  hal.files[ACTIVE_COURSE_PATH] = bytes;
  intent.activePack = declaration.manifest;
  const auto different = hal.files;
  EXPECT_FALSE(verifier->verify(intent, originalPath));
  EXPECT_EQ(hal.files, different);
  EXPECT_FALSE(verifier->verified(intent));
  EXPECT_FALSE(parser->isOpen());
}

TEST_F(HalCourseTransferTest, UnboundReviewedFileLoansOnlyHashVerifiedLearnerCopies) {
  Digest hash{};
  Identity reader{};
  const auto prefix = prepareUnboundBackupReview(hash, reader);
  ASSERT_FALSE(prefix.empty());
  auto permission = [](void* raw) { return *static_cast<bool*>(raw); };
  bool permitted = true;
  {
    auto backups = makeUniqueNoThrow<HalCourseBaselineReviewBackup>(scratch, permission, &permitted);
    ASSERT_TRUE(backups);
    ASSERT_TRUE(backups->preserveUnbound(hash, reader, generation, declaration.manifest.logicalIdentity));
    ASSERT_TRUE(backups->closeReaders());
  }
  UnboundCourseMigrationRequest request;
  request.original = {generation, declaration.state.owner, declaration.state.transaction, declaration.manifest, hash};
  auto reviewed = makeUniqueNoThrow<HalUnboundCourseReviewedFile>(reader, generation, scratch, permission, &permitted);
  ASSERT_TRUE(reviewed);
  const auto files = inventory_hal_test::state.files;
  ASSERT_EQ(reviewed->open(request, "items.bin"), UnboundReviewedFileResult::Present);
  auto* file = reviewed->borrowed();
  ASSERT_NE(file, nullptr);
  uint8_t byte = 0;
  ASSERT_EQ(file->read(&byte, 1), 1);
  EXPECT_EQ(byte, 17);
  EXPECT_EQ(file->fileSize64(), 12000u);
  permitted = false;
  EXPECT_EQ(reviewed->borrowed(), nullptr);
  permitted = true;
  EXPECT_EQ(reviewed->borrowed(), nullptr);
  EXPECT_EQ(reviewed->open(request, "usage.bin"), UnboundReviewedFileResult::Invalid);
  EXPECT_EQ(reviewed->borrowed(), nullptr);
  EXPECT_EQ(reviewed->open(request, "session.bin"), UnboundReviewedFileResult::Missing);
  EXPECT_EQ(reviewed->borrowed(), nullptr);
  EXPECT_EQ(inventory_hal_test::state.files, files);
  // Historical evidence survives disappearance of mutable global sources.
  inventory_hal_test::state.files.erase("/tinta/ITEMS.BIN");
  ASSERT_EQ(reviewed->open(request, "items.bin"), UnboundReviewedFileResult::Present);
  ASSERT_NE(reviewed->borrowed(), nullptr);
  ASSERT_TRUE(reviewed->closeReaders());
  inventory_hal_test::state.files.at(prefix + "-00")[999] ^= 1;
  const auto damaged = inventory_hal_test::state.files;
  EXPECT_EQ(reviewed->open(request, "items.bin"), UnboundReviewedFileResult::Unavailable);
  EXPECT_EQ(reviewed->borrowed(), nullptr);
  EXPECT_EQ(inventory_hal_test::state.files, damaged);
}

TEST_F(HalCourseTransferTest, UnboundReviewedFileRefusesForeignContextAndPendingCopy) {
  Digest hash{};
  Identity reader{};
  const auto prefix = prepareUnboundBackupReview(hash, reader);
  ASSERT_FALSE(prefix.empty());
  {
    auto backups = makeUniqueNoThrow<HalCourseBaselineReviewBackup>(scratch, [](void*) { return true; }, nullptr);
    ASSERT_TRUE(backups);
    ASSERT_TRUE(backups->preserveUnbound(hash, reader, generation, declaration.manifest.logicalIdentity));
    ASSERT_TRUE(backups->closeReaders());
  }
  UnboundCourseMigrationRequest request;
  request.original = {generation, declaration.state.owner, declaration.state.transaction, declaration.manifest, hash};
  auto wrong = reader;
  wrong[0] ^= 2;
  auto foreign =
      makeUniqueNoThrow<HalUnboundCourseReviewedFile>(wrong, generation, scratch, [](void*) { return true; }, nullptr);
  ASSERT_TRUE(foreign);
  EXPECT_EQ(foreign->open(request, "items.bin"), UnboundReviewedFileResult::Unavailable);
  EXPECT_EQ(foreign->borrowed(), nullptr);
  foreign.reset();
  auto reviewed =
      makeUniqueNoThrow<HalUnboundCourseReviewedFile>(reader, generation, scratch, [](void*) { return true; }, nullptr);
  ASSERT_TRUE(reviewed);
  auto changed = request;
  changed.original.generation[0] ^= 2;
  EXPECT_EQ(reviewed->open(changed, "items.bin"), UnboundReviewedFileResult::Invalid);
  inventory_hal_test::state.files[prefix + "-00.tmp"] = {1};
  const auto files = inventory_hal_test::state.files;
  EXPECT_EQ(reviewed->open(request, "items.bin"), UnboundReviewedFileResult::Unavailable);
  EXPECT_EQ(reviewed->borrowed(), nullptr);
  EXPECT_EQ(inventory_hal_test::state.files, files);
}

TEST_F(HalCourseTransferTest, UnboundItemInspectionValidatesCopiedUidsAndPreservesFailureOutput) {
  auto parser = makeUniqueNoThrow<tinta::core::pack::Pack>();
  ASSERT_TRUE(parser);
  ASSERT_EQ(parser->open(bytes.data(), bytes.size()), tinta::core::pack::PackStatus::Ok);
  const auto uid = parser->uidAt(0);
  parser->close();
  for (const bool known : {true, false}) {
    inventory_hal_test::state = {};
    inventory_hal_test::state.enumerateFileMap = true;
    auto& hal = inventory_hal_test::state;
    hal.directories["/tinta"] = {};
    hal.directories[TRANSFER_DIRECTORY] = {};
    hal.files[ACTIVE_COURSE_PATH] = bytes;
    auto& items = hal.files["/tinta/items.bin"];
    items.resize(1040, 0);
    for (unsigned slot = 0; slot < 2; ++slot) {
      auto* header = items.data() + slot * 512;
      std::memcpy(header, "TIS1", 4);
      binary_record::putU16(header + 4, 1);
      binary_record::putU16(header + 6, 80);
      binary_record::putU32(header + 8, slot + 1);
      binary_record::putU32(header + 12, 1);
      binary_record::putU32(header + 16, 9);
      binary_record::putU32(header + 76, binary_record::crc32(header, 76));
    }
    tinta::core::ItemState::fresh(known ? uid : UINT32_MAX - 1).encode(items.data() + 1024);
    Identity reader{};
    reader.fill(51);
    Digest hash{};
    {
      auto capture = makeUniqueNoThrow<HalCourseBaselineReviewCapture>(scratch, [](void*) { return true; }, nullptr);
      ASSERT_TRUE(capture);
      ASSERT_EQ(capture->capture(reader, generation, declaration.manifest.logicalIdentity),
                CourseBaselineReviewResult::Ok);
      hash = *capture->hash();
      const std::vector<uint8_t> encoded(capture->bytes().begin(), capture->bytes().end());
      capture.reset();
      auto roster = makeUniqueNoThrow<HalCourseBaselineReviewStore>(
          std::span(scratch).subspan(COURSE_BASELINE_REVIEW_MAX_SIZE), [](void*) { return true; }, nullptr);
      ASSERT_TRUE(roster);
      ASSERT_EQ(roster->publish(encoded, hash), CourseBaselineReviewStoreResult::Ok);
    }
    {
      auto backups = makeUniqueNoThrow<HalCourseBaselineReviewBackup>(scratch, [](void*) { return true; }, nullptr);
      ASSERT_TRUE(backups);
      ASSERT_TRUE(backups->preserveUnbound(hash, reader, generation, declaration.manifest.logicalIdentity));
      ASSERT_TRUE(backups->closeReaders());
    }
    UnboundCourseMigrationRequest request;
    request.original = {generation, declaration.state.owner, declaration.state.transaction, declaration.manifest, hash};
    auto reviewed = makeUniqueNoThrow<HalUnboundCourseReviewedFile>(
        reader, generation, scratch, [](void*) { return true; }, nullptr);
    ASSERT_TRUE(reviewed);
    HalInventoryIndexStorage packStorage;
    ASSERT_TRUE(packStorage.open(ACTIVE_COURSE_PATH));
    StoredCourseSource original(packStorage);
    ASSERT_TRUE(original.attach());
    UnboundCourseItemReport report;
    report.committedRecords = 123;
    const auto files = hal.files;
    EXPECT_EQ(inspectUnboundCourseItems(
                  *reviewed, request, original, scratch, report, [](void*) { return true; }, nullptr),
              known);
    if (known) {
      EXPECT_TRUE(report.present);
      EXPECT_EQ(report.catalog.mapped, 1u);
      EXPECT_EQ(report.committedRecords, 9u);
    } else {
      EXPECT_FALSE(report.present);
      EXPECT_EQ(report.committedRecords, 123u);
    }
    EXPECT_EQ(reviewed->borrowed(), nullptr);
    EXPECT_EQ(hal.files, files);
  }
}

TEST_F(HalCourseTransferTest, UnboundReviewInspectionChecksCommittedCountUidsAndUndoSyntax) {
  auto parser = makeUniqueNoThrow<tinta::core::pack::Pack>();
  ASSERT_TRUE(parser);
  ASSERT_EQ(parser->open(bytes.data(), bytes.size()), tinta::core::pack::PackStatus::Ok);
  const auto uid = parser->uidAt(0);
  parser->close();
  for (unsigned fault = 0; fault < 6; ++fault) {
    inventory_hal_test::state = {};
    inventory_hal_test::state.enumerateFileMap = true;
    auto& hal = inventory_hal_test::state;
    hal.directories["/tinta"] = {};
    hal.directories[TRANSFER_DIRECTORY] = {};
    hal.files[ACTIVE_COURSE_PATH] = bytes;
    auto& items = hal.files["/tinta/items.bin"];
    items.resize(1040, 0);
    for (unsigned slot = 0; slot < 2; ++slot) {
      auto* header = items.data() + slot * 512;
      std::memcpy(header, "TIS1", 4);
      binary_record::putU16(header + 4, 1);
      binary_record::putU16(header + 6, 80);
      binary_record::putU32(header + 8, slot + 1);
      binary_record::putU32(header + 12, 1);
      binary_record::putU32(header + 16, fault == 0 || fault == 5 ? 3 : 1);
      binary_record::putU32(header + 76, binary_record::crc32(header, 76));
    }
    tinta::core::ItemState::fresh(uid).encode(items.data() + 1024);
    if (fault != 1) {
      auto& log = hal.files["/tinta/reviews.log"];
      log.resize(fault == 0 ? 39 : 12, 0);
      binary_record::putU32(log.data(), fault == 2 ? UINT32_MAX - 1 : uid);
      log[10] = fault == 3 ? 8 : 3;
      if (fault == 0) {
        binary_record::putU32(log.data() + 12, uid);
        log[22] = 8;
        binary_record::putU32(log.data() + 24, uid);
        log[34] = 16;
        log[35] = 1;
      }
      if (fault == 4) log.pop_back();
    }
    Identity reader{};
    reader.fill(51);
    Digest hash{};
    {
      auto capture = makeUniqueNoThrow<HalCourseBaselineReviewCapture>(scratch, [](void*) { return true; }, nullptr);
      ASSERT_TRUE(capture);
      ASSERT_EQ(capture->capture(reader, generation, declaration.manifest.logicalIdentity),
                CourseBaselineReviewResult::Ok);
      hash = *capture->hash();
      const std::vector<uint8_t> encoded(capture->bytes().begin(), capture->bytes().end());
      capture.reset();
      auto roster = makeUniqueNoThrow<HalCourseBaselineReviewStore>(
          std::span(scratch).subspan(COURSE_BASELINE_REVIEW_MAX_SIZE), [](void*) { return true; }, nullptr);
      ASSERT_TRUE(roster);
      ASSERT_EQ(roster->publish(encoded, hash), CourseBaselineReviewStoreResult::Ok);
    }
    {
      auto backups = makeUniqueNoThrow<HalCourseBaselineReviewBackup>(scratch, [](void*) { return true; }, nullptr);
      ASSERT_TRUE(backups);
      ASSERT_TRUE(backups->preserveUnbound(hash, reader, generation, declaration.manifest.logicalIdentity));
      ASSERT_TRUE(backups->closeReaders());
    }
    UnboundCourseMigrationRequest request;
    request.original = {generation, declaration.state.owner, declaration.state.transaction, declaration.manifest, hash};
    auto reviewed = makeUniqueNoThrow<HalUnboundCourseReviewedFile>(
        reader, generation, scratch, [](void*) { return true; }, nullptr);
    ASSERT_TRUE(reviewed);
    HalInventoryIndexStorage packStorage;
    ASSERT_TRUE(packStorage.open(ACTIVE_COURSE_PATH));
    StoredCourseSource original(packStorage);
    ASSERT_TRUE(original.attach());
    UnboundCourseItemReport itemReport;
    ASSERT_TRUE(inspectUnboundCourseItems(
        *reviewed, request, original, scratch, itemReport, [](void*) { return true; }, nullptr));
    UnboundCourseReviewReport report;
    report.journal.records = 123;
    const auto files = hal.files;
    EXPECT_EQ(inspectUnboundCourseReviews(
                  *reviewed, request, original, itemReport, scratch, report, [](void*) { return true; }, nullptr),
              fault == 0);
    if (!fault) {
      EXPECT_TRUE(report.present);
      EXPECT_EQ(report.journal.records, 3u);
      EXPECT_EQ(report.journal.zeroTailBytes, 3u);
    } else {
      EXPECT_FALSE(report.present);
      EXPECT_EQ(report.journal.records, 123u);
    }
    EXPECT_EQ(reviewed->borrowed(), nullptr);
    EXPECT_EQ(hal.files, files);
  }
}

TEST_F(HalCourseTransferTest, UnboundProfileInspectionPreservesRepairsAndRejectsInvalidLessons) {
  auto parser = makeUniqueNoThrow<tinta::core::pack::Pack>();
  ASSERT_TRUE(parser);
  ASSERT_EQ(parser->open(bytes.data(), bytes.size()), tinta::core::pack::PackStatus::Ok);
  for (unsigned fault = 0; fault < 5; ++fault) {
    inventory_hal_test::state = {};
    inventory_hal_test::state.enumerateFileMap = true;
    auto& hal = inventory_hal_test::state;
    hal.directories["/tinta"] = {};
    hal.directories[TRANSFER_DIRECTORY] = {};
    hal.files[ACTIVE_COURSE_PATH] = bytes;
    hal.files["/tinta/items.bin"] = {1};
    if (fault != 4) {
      tinta::core::Profile profile;
      if (fault == 2) profile.currentLesson = UINT16_MAX;
      if (fault == 3) profile.frontlightBrightness = 255;
      auto& encoded = hal.files["/tinta/profile.bin"];
      encoded.resize(tinta::core::Profile::kEncodedSize);
      profile.encode(encoded.data());
      if (fault == 1) encoded.back() ^= 1;
    }
    Identity reader{};
    reader.fill(51);
    Digest hash{};
    {
      auto capture = makeUniqueNoThrow<HalCourseBaselineReviewCapture>(scratch, [](void*) { return true; }, nullptr);
      ASSERT_TRUE(capture);
      ASSERT_EQ(capture->capture(reader, generation, declaration.manifest.logicalIdentity),
                CourseBaselineReviewResult::Ok);
      hash = *capture->hash();
      const std::vector<uint8_t> encoded(capture->bytes().begin(), capture->bytes().end());
      capture.reset();
      auto roster = makeUniqueNoThrow<HalCourseBaselineReviewStore>(
          std::span(scratch).subspan(COURSE_BASELINE_REVIEW_MAX_SIZE), [](void*) { return true; }, nullptr);
      ASSERT_TRUE(roster);
      ASSERT_EQ(roster->publish(encoded, hash), CourseBaselineReviewStoreResult::Ok);
    }
    {
      auto backups = makeUniqueNoThrow<HalCourseBaselineReviewBackup>(scratch, [](void*) { return true; }, nullptr);
      ASSERT_TRUE(backups);
      ASSERT_TRUE(backups->preserveUnbound(hash, reader, generation, declaration.manifest.logicalIdentity));
      ASSERT_TRUE(backups->closeReaders());
    }
    UnboundCourseMigrationRequest request;
    request.original = {generation, declaration.state.owner, declaration.state.transaction, declaration.manifest, hash};
    auto reviewed = makeUniqueNoThrow<HalUnboundCourseReviewedFile>(
        reader, generation, scratch, [](void*) { return true; }, nullptr);
    ASSERT_TRUE(reviewed);

    UnboundCourseProfileReport report;
    report.profile.newPerDay = 123;
    const auto files = hal.files;
    EXPECT_EQ(inspectUnboundCourseProfile(
                  *reviewed, request, *parser, scratch, report, [](void*) { return true; }, nullptr),
              fault == 0 || fault == 3 || fault == 4);
    if (fault == 1 || fault == 2) {
      EXPECT_EQ(report.profile.newPerDay, 123);
      EXPECT_FALSE(report.present);
    } else {
      EXPECT_EQ(report.present, fault != 4);
      EXPECT_EQ(report.status, fault == 4   ? tinta::core::Profile::LoadResult::Defaults
                               : fault == 3 ? tinta::core::Profile::LoadResult::Upgraded
                                            : tinta::core::Profile::LoadResult::Loaded);
    }
    EXPECT_EQ(reviewed->borrowed(), nullptr);
    EXPECT_EQ(hal.files, files);
  }
}

TEST_F(HalCourseTransferTest, UnboundMarkInspectionChecksFrozenMembershipAndPreservesFailureOutput) {
  auto parser = makeUniqueNoThrow<tinta::core::pack::Pack>();
  ASSERT_TRUE(parser);
  ASSERT_EQ(parser->open(bytes.data(), bytes.size()), tinta::core::pack::PackStatus::Ok);
  const auto uid = parser->uidAt(0);
  tinta::core::pack::Story story;
  ASSERT_TRUE(parser->story(0, story));
  uint32_t storyKey = 0;
  ASSERT_TRUE(tintaLegacyStoryKey(*parser, story, storyKey));
  for (unsigned fault = 0; fault < 6; ++fault) {
    inventory_hal_test::state = {};
    inventory_hal_test::state.enumerateFileMap = true;
    auto& hal = inventory_hal_test::state;
    hal.directories["/tinta"] = {};
    hal.directories[TRANSFER_DIRECTORY] = {};
    hal.files[ACTIVE_COURSE_PATH] = bytes;
    hal.files["/tinta/items.bin"] = {1};
    const bool readings = fault == 4;
    if (fault != 5) {
      auto& log = hal.files[readings ? "/tinta/read.bin" : "/tinta/starred.bin"];
      log.resize(28, 0);
      std::memcpy(log.data(), "TMK1", 4);
      const uint32_t key = readings ? storyKey : fault == 2 ? UINT32_MAX - 1 : uid;
      for (unsigned index = 0; index < 3; ++index) {
        auto* record = log.data() + 4 + index * 8;
        binary_record::putU32(record, key);
        record[4] = index == 1 ? 2 : 1;
        binary_record::putU16(record + 6, uint16_t(binary_record::crc32(record, 6)));
      }
      if (fault == 1) log.back() ^= 1;
      if (fault == 3) log.pop_back();
    }
    Identity reader{};
    reader.fill(51);
    Digest hash{};
    {
      auto capture = makeUniqueNoThrow<HalCourseBaselineReviewCapture>(scratch, [](void*) { return true; }, nullptr);
      ASSERT_TRUE(capture);
      ASSERT_EQ(capture->capture(reader, generation, declaration.manifest.logicalIdentity),
                CourseBaselineReviewResult::Ok);
      hash = *capture->hash();
      const std::vector<uint8_t> encoded(capture->bytes().begin(), capture->bytes().end());
      capture.reset();
      auto roster = makeUniqueNoThrow<HalCourseBaselineReviewStore>(
          std::span(scratch).subspan(COURSE_BASELINE_REVIEW_MAX_SIZE), [](void*) { return true; }, nullptr);
      ASSERT_TRUE(roster);
      ASSERT_EQ(roster->publish(encoded, hash), CourseBaselineReviewStoreResult::Ok);
    }
    {
      auto backups = makeUniqueNoThrow<HalCourseBaselineReviewBackup>(scratch, [](void*) { return true; }, nullptr);
      ASSERT_TRUE(backups);
      ASSERT_TRUE(backups->preserveUnbound(hash, reader, generation, declaration.manifest.logicalIdentity));
      ASSERT_TRUE(backups->closeReaders());
    }
    UnboundCourseMigrationRequest request;
    request.original = {generation, declaration.state.owner, declaration.state.transaction, declaration.manifest, hash};
    auto reviewed = makeUniqueNoThrow<HalUnboundCourseReviewedFile>(
        reader, generation, scratch, [](void*) { return true; }, nullptr);
    ASSERT_TRUE(reviewed);

    HalInventoryIndexStorage packStorage;
    ASSERT_TRUE(packStorage.open(ACTIVE_COURSE_PATH));
    StoredCourseSource original(packStorage);
    ASSERT_TRUE(original.attach());
    UnboundCourseMarkReport report;
    report.catalog.matched = 123;
    const auto files = hal.files;
    EXPECT_EQ(inspectUnboundCourseMarks(
                  *reviewed, request, original, *parser,
                  readings ? UnboundCourseMarkKind::Readings : UnboundCourseMarkKind::Stars, scratch, report,
                  [](void*) { return true; }, nullptr),
              fault == 0 || fault == 4 || fault == 5);
    if (fault == 1 || fault == 2 || fault == 3) {
      EXPECT_FALSE(report.present);
      EXPECT_EQ(report.catalog.matched, 123);
    } else {
      EXPECT_EQ(report.present, fault != 5);
      EXPECT_EQ(report.catalog.matched, fault == 5 ? 0 : 1);
      EXPECT_EQ(report.catalog.retired, 0);
    }
    EXPECT_EQ(reviewed->borrowed(), nullptr);
    EXPECT_EQ(hal.files, files);
  }
}

TEST_F(HalCourseTransferTest, UnboundDayInspectionAcceptsUnsortedSplitsAndRejectsInconsistentDays) {
  for (unsigned fault = 0; fault < 6; ++fault) {
    inventory_hal_test::state = {};
    inventory_hal_test::state.enumerateFileMap = true;
    auto& hal = inventory_hal_test::state;
    hal.directories["/tinta"] = {};
    hal.directories[TRANSFER_DIRECTORY] = {};
    hal.files[ACTIVE_COURSE_PATH] = bytes;
    hal.files["/tinta/items.bin"] = {1};
    if (fault != 5) {
      auto& log = hal.files["/tinta/days.bin"];
      log.resize(40, 0);
      std::memcpy(log.data(), "TDL1", 4);
      for (unsigned index = 0; index < 3; ++index) {
        auto* record = log.data() + 4 + index * 12;
        binary_record::putU16(record, index == 1 ? 2 : UINT16_MAX);
        binary_record::putU16(record + 2, index == 2 ? 0 : 3);
        binary_record::putU16(record + 4, index == 2 ? 2 : fault == 3 && index == 1 ? 4 : 1);
        binary_record::putU16(record + 6, index == 2 ? 0 : 1);
        binary_record::putU16(record + 8, 10);
        binary_record::putU16(record + 10, uint16_t(binary_record::crc32(record, 10)));
      }
      if (fault == 1) log.back() ^= 1;
      if (fault == 2) log.pop_back();
      if (fault == 4) log[0] = 'X';
    }
    Identity reader{};
    reader.fill(51);
    Digest hash{};
    {
      auto capture = makeUniqueNoThrow<HalCourseBaselineReviewCapture>(scratch, [](void*) { return true; }, nullptr);
      ASSERT_TRUE(capture);
      ASSERT_EQ(capture->capture(reader, generation, declaration.manifest.logicalIdentity),
                CourseBaselineReviewResult::Ok);
      hash = *capture->hash();
      const std::vector<uint8_t> encoded(capture->bytes().begin(), capture->bytes().end());
      capture.reset();
      auto roster = makeUniqueNoThrow<HalCourseBaselineReviewStore>(
          std::span(scratch).subspan(COURSE_BASELINE_REVIEW_MAX_SIZE), [](void*) { return true; }, nullptr);
      ASSERT_TRUE(roster);
      ASSERT_EQ(roster->publish(encoded, hash), CourseBaselineReviewStoreResult::Ok);
    }
    {
      auto backups = makeUniqueNoThrow<HalCourseBaselineReviewBackup>(scratch, [](void*) { return true; }, nullptr);
      ASSERT_TRUE(backups);
      ASSERT_TRUE(backups->preserveUnbound(hash, reader, generation, declaration.manifest.logicalIdentity));
      ASSERT_TRUE(backups->closeReaders());
    }
    UnboundCourseMigrationRequest request;
    request.original = {generation, declaration.state.owner, declaration.state.transaction, declaration.manifest, hash};
    auto reviewed = makeUniqueNoThrow<HalUnboundCourseReviewedFile>(
        reader, generation, scratch, [](void*) { return true; }, nullptr);
    ASSERT_TRUE(reviewed);

    UnboundCourseDayReport report;
    report.records = 123;
    const auto files = hal.files;
    EXPECT_EQ(inspectUnboundCourseDays(
                  *reviewed, request, scratch, report, [](void*) { return true; }, nullptr),
              fault == 0 || fault == 5);
    if (fault > 0 && fault < 5) {
      EXPECT_FALSE(report.present);
      EXPECT_EQ(report.records, 123u);
    } else {
      EXPECT_EQ(report.present, fault != 5);
      EXPECT_EQ(report.records, fault == 5 ? 0u : 3u);
      EXPECT_EQ(report.days, fault == 5 ? 0u : 2u);
      if (!fault) {
        EXPECT_EQ(report.totals, (std::array<uint32_t, 4>{6, 4, 2, 30}));
      }
    }
    EXPECT_EQ(reviewed->borrowed(), nullptr);
    EXPECT_EQ(hal.files, files);
  }
}

TEST_F(HalCourseTransferTest, UnboundSessionInspectionPreservesSnapshotEvidenceAndChecksQueueReferences) {
  auto parser = makeUniqueNoThrow<tinta::core::pack::Pack>();
  ASSERT_TRUE(parser);
  ASSERT_EQ(parser->open(bytes.data(), bytes.size()), tinta::core::pack::PackStatus::Ok);
  const auto uid = parser->uidAt(0);
  for (unsigned fault = 0; fault < 6; ++fault) {
    inventory_hal_test::state = {};
    inventory_hal_test::state.enumerateFileMap = true;
    auto& hal = inventory_hal_test::state;
    hal.directories["/tinta"] = {};
    hal.directories[TRANSFER_DIRECTORY] = {};
    hal.files[ACTIVE_COURSE_PATH] = bytes;
    hal.files["/tinta/profile.bin"] = {1};
    if (fault != 5) {
      auto& saved = hal.files["/tinta/session.bin"];
      saved.resize(103, 0);
      std::memcpy(saved.data(), "TSES", 4);
      binary_record::putU16(saved.data() + 4, 3);
      saved[6] = 1;
      saved[7] = fault == 3 ? 255 : 1;
      binary_record::putU16(saved.data() + 8, 57);
      auto* controller = saved.data() + 10;
      binary_record::putU32(controller, fault == 4 ? 1 : 0);
      binary_record::putU16(controller + 16, 37);
      auto* queue = controller + 20;
      std::memcpy(queue, "TSQ1", 4);
      binary_record::putU16(queue + 4, 2);
      binary_record::putU16(queue + 6, 1);
      binary_record::putU16(queue + 8, 99);
      binary_record::putU32(queue + 28, fault == 2 ? UINT32_MAX - 1 : uid);
      binary_record::putU32(queue + 33, binary_record::crc32(queue, 33));
      saved[67] = 1;
      binary_record::putU32(saved.data() + 99, binary_record::crc32(saved.data(), 99));
      if (fault == 1) saved.back() ^= 1;
    }
    Identity reader{};
    reader.fill(51);
    Digest hash{};
    {
      auto capture = makeUniqueNoThrow<HalCourseBaselineReviewCapture>(scratch, [](void*) { return true; }, nullptr);
      ASSERT_TRUE(capture);
      ASSERT_EQ(capture->capture(reader, generation, declaration.manifest.logicalIdentity),
                CourseBaselineReviewResult::Ok);
      hash = *capture->hash();
      const std::vector<uint8_t> encoded(capture->bytes().begin(), capture->bytes().end());
      capture.reset();
      auto roster = makeUniqueNoThrow<HalCourseBaselineReviewStore>(
          std::span(scratch).subspan(COURSE_BASELINE_REVIEW_MAX_SIZE), [](void*) { return true; }, nullptr);
      ASSERT_TRUE(roster);
      ASSERT_EQ(roster->publish(encoded, hash), CourseBaselineReviewStoreResult::Ok);
    }
    {
      auto backups = makeUniqueNoThrow<HalCourseBaselineReviewBackup>(scratch, [](void*) { return true; }, nullptr);
      ASSERT_TRUE(backups);
      ASSERT_TRUE(backups->preserveUnbound(hash, reader, generation, declaration.manifest.logicalIdentity));
      ASSERT_TRUE(backups->closeReaders());
    }
    UnboundCourseMigrationRequest request;
    request.original = {generation, declaration.state.owner, declaration.state.transaction, declaration.manifest, hash};
    auto reviewed = makeUniqueNoThrow<HalUnboundCourseReviewedFile>(
        reader, generation, scratch, [](void*) { return true; }, nullptr);
    ASSERT_TRUE(reviewed);

    HalInventoryIndexStorage packStorage;
    ASSERT_TRUE(packStorage.open(ACTIVE_COURSE_PATH));
    StoredCourseSource original(packStorage);
    ASSERT_TRUE(original.attach());
    UnboundCourseItemReport itemReport;
    UnboundCourseReviewReport reviewReport;
    ASSERT_TRUE(inspectUnboundCourseItems(
        *reviewed, request, original, scratch, itemReport, [](void*) { return true; }, nullptr));
    ASSERT_TRUE(inspectUnboundCourseReviews(
        *reviewed, request, original, itemReport, scratch, reviewReport, [](void*) { return true; }, nullptr));
    UnboundCourseSessionReport report;
    report.session.queued = 123;
    const auto files = hal.files;
    EXPECT_EQ(inspectUnboundCourseSession(
                  *reviewed, request, original, *parser, itemReport, reviewReport, scratch, report,
                  [](void*) { return true; }, nullptr),
              fault == 0 || fault == 4 || fault == 5);
    if (fault > 0 && fault < 4) {
      EXPECT_FALSE(report.present);
      EXPECT_EQ(report.session.queued, 123);
    } else {
      EXPECT_EQ(report.present, fault != 5);
      EXPECT_EQ(report.session.queued, fault == 5 ? 0 : 1);
      EXPECT_EQ(report.session.hasSnapshot, fault != 5);
      EXPECT_EQ(report.session.journalChanged, fault == 4);
      EXPECT_EQ(report.session.mapped, fault == 5 ? 0 : 1);
    }
    EXPECT_EQ(reviewed->borrowed(), nullptr);
    EXPECT_EQ(hal.files, files);
  }
}

TEST_F(HalCourseTransferTest, UnboundLearnerInspectionRequiresWholeCohortAndRevokesItsReport) {
  auto parser = makeUniqueNoThrow<tinta::core::pack::Pack>();
  ASSERT_TRUE(parser);
  ASSERT_EQ(parser->open(bytes.data(), bytes.size()), tinta::core::pack::PackStatus::Ok);
  for (unsigned fault = 0; fault < 9; ++fault) {
    inventory_hal_test::state = {};
    inventory_hal_test::state.enumerateFileMap = true;
    auto& hal = inventory_hal_test::state;
    hal.directories["/tinta"] = {};
    hal.directories[TRANSFER_DIRECTORY] = {};
    hal.files[ACTIVE_COURSE_PATH] = bytes;
    tinta::core::Profile profile;
    auto& encoded = hal.files["/tinta/profile.bin"];
    encoded.resize(tinta::core::Profile::kEncodedSize);
    profile.encode(encoded.data());
    if (fault == 0) {
      tinta::core::pack::Story story;
      ASSERT_TRUE(parser->story(0, story));
      uint32_t key = 0;
      ASSERT_TRUE(tintaLegacyStoryKey(*parser, story, key));
      auto& marks = hal.files["/tinta/read.bin"];
      marks.resize(12, 0);
      std::memcpy(marks.data(), "TMK1", 4);
      binary_record::putU32(marks.data() + 4, key);
      marks[8] = 1;
      binary_record::putU16(marks.data() + 10, uint16_t(binary_record::crc32(marks.data() + 4, 6)));
    }
    if (fault == 7) hal.files["/tinta/read.bin"] = {'T', 'M', 'K', '1'};
    if (fault == 8) {
      auto& marks = hal.files["/tinta/read.bin"];
      marks.resize(12, 0);
      std::memcpy(marks.data(), "TMK1", 4);
      binary_record::putU32(marks.data() + 4, 123);
      marks[8] = 1;
      binary_record::putU16(marks.data() + 10, uint16_t(binary_record::crc32(marks.data() + 4, 6)));
    }
    if (fault == 2) encoded.back() ^= 1;
    if (fault == 3) hal.files["/tinta/items.bin"] = {1};
    Identity reader{};
    reader.fill(51);
    Digest hash{};
    {
      auto capture = makeUniqueNoThrow<HalCourseBaselineReviewCapture>(scratch, [](void*) { return true; }, nullptr);
      ASSERT_TRUE(capture);
      ASSERT_EQ(capture->capture(reader, generation, declaration.manifest.logicalIdentity),
                CourseBaselineReviewResult::Ok);
      hash = *capture->hash();
      const std::vector<uint8_t> encoded(capture->bytes().begin(), capture->bytes().end());
      capture.reset();
      auto roster = makeUniqueNoThrow<HalCourseBaselineReviewStore>(
          std::span(scratch).subspan(COURSE_BASELINE_REVIEW_MAX_SIZE), [](void*) { return true; }, nullptr);
      ASSERT_TRUE(roster);
      ASSERT_EQ(roster->publish(encoded, hash), CourseBaselineReviewStoreResult::Ok);
    }
    {
      auto backups = makeUniqueNoThrow<HalCourseBaselineReviewBackup>(scratch, [](void*) { return true; }, nullptr);
      ASSERT_TRUE(backups);
      ASSERT_TRUE(backups->preserveUnbound(hash, reader, generation, declaration.manifest.logicalIdentity));
      ASSERT_TRUE(backups->closeReaders());
    }
    UnboundCourseMigrationRequest request;
    request.original = {generation, declaration.state.owner, declaration.state.transaction, declaration.manifest, hash};
    auto reviewed = makeUniqueNoThrow<HalUnboundCourseReviewedFile>(
        reader, generation, scratch, [](void*) { return true; }, nullptr);
    ASSERT_TRUE(reviewed);

    auto backups = makeUniqueNoThrow<HalCourseBaselineReviewBackup>(scratch, [](void*) { return true; }, nullptr);
    ASSERT_TRUE(backups);
    if (fault == 1) {
      bool damaged = false;
      for (auto& [path, file] : hal.files) {
        if (path.find("course-review-state-") != std::string::npos && !file.empty()) {
          file.back() ^= 1;
          damaged = true;
          break;
        }
      }
      ASSERT_TRUE(damaged);
    }
    if (fault == 4) hal.files["/tinta/profile.bin"] = {9};
    if (fault == 5) request.original.manifest.contentHash[0] ^= 1;
    if (fault == 6) hal.files[ACTIVE_COURSE_PATH].back() ^= 1;
    HalInventoryIndexStorage packStorage;
    ASSERT_TRUE(packStorage.open(ACTIVE_COURSE_PATH));
    StoredCourseSource original(packStorage);
    ASSERT_TRUE(original.attach());
    struct PermissionContext {
      bool permitted = true, checking = false, nested = false;
      uint32_t probes = 0;
      HalUnboundCourseLearnerInspection* owner = nullptr;
      const UnboundCourseMigrationRequest* request = nullptr;
      tinta::core::pack::PackSource* source = nullptr;
      tinta::core::pack::Pack* pack = nullptr;
    } permission;
    permission.request = &request;
    permission.source = &original;
    permission.pack = parser.get();
    auto inspection = makeUniqueNoThrow<HalUnboundCourseLearnerInspection>(
        reader, generation, *reviewed, *backups, scratch,
        [](void* context) {
          auto& permission = *static_cast<PermissionContext*>(context);
          if (permission.checking && !permission.nested) {
            permission.nested = true;
            EXPECT_FALSE(permission.owner->inspect(*permission.request, *permission.source, *permission.pack));
            EXPECT_EQ(permission.owner->report(*permission.request), nullptr);
            ++permission.probes;
            permission.nested = false;
          }
          return permission.permitted;
        },
        &permission);
    ASSERT_TRUE(inspection);
    permission.owner = inspection.get();
    permission.checking = true;
    const auto files = hal.files;
    EXPECT_EQ(inspection->inspect(request, original, *parser), fault == 0 || fault == 4 || fault == 7 || fault == 8);
    permission.checking = false;
    EXPECT_GT(permission.probes, 0u);
    if (fault == 0 || fault == 4 || fault == 7 || fault == 8) {
      auto foreign = request;
      foreign.original.transaction[0] ^= 1;
      EXPECT_EQ(inspection->report(foreign), nullptr);
      foreign = request;
      foreign.original.owner[0] ^= 1;
      EXPECT_EQ(inspection->report(foreign), nullptr);
      foreign = request;
      foreign.original.reviewHash[0] ^= 1;
      EXPECT_EQ(inspection->report(foreign), nullptr);
      permission.checking = true;
      const auto probes = permission.probes;
      ASSERT_NE(inspection->report(request), nullptr);
      EXPECT_GT(permission.probes, probes);
      permission.checking = false;
      EXPECT_TRUE(inspection->report(request)->profile.present);
      EXPECT_FALSE(inspection->report(request)->items.present);
      EXPECT_FALSE(inspection->report(request)->session.present);
      if (fault == 0 || fault == 4 || fault == 7 || fault == 8) {
        UnboundCourseMigrationIntent intent;
        intent.reader = reader;
        intent.request = request;
        intent.activePack = declaration.manifest;
        auto installedParser = makeUniqueNoThrow<tinta::core::pack::Pack>();
        ASSERT_TRUE(installedParser);
        auto oldReader =
            makeUniqueNoThrow<HalUnboundCoursePackReader>(scratch, *parser, [](void*) { return true; }, nullptr);
        auto newReader = makeUniqueNoThrow<HalUnboundCoursePackReader>(
            scratch, *installedParser, [](void*) { return true; }, nullptr);
        ASSERT_TRUE(oldReader);
        ASSERT_TRUE(newReader);
        ASSERT_TRUE(oldReader->open(intent, UnboundPackRole::Original, ACTIVE_COURSE_PATH));
        ASSERT_TRUE(newReader->open(intent, UnboundPackRole::Installed));
        TintaLegacyLessonMapping mapped;
        ASSERT_TRUE(mapUnboundCourseBoundLessons(
            *inspection, *oldReader, *newReader, intent, scratch, mapped, [](void*) { return true; }, nullptr));
        EXPECT_EQ(mapped.currentLesson, 0);
        UnboundCourseReadingMappingReport readings;
        ASSERT_TRUE(mapUnboundCourseBoundReadings(
            *inspection, *reviewed, *oldReader, *newReader, intent, scratch, readings, [](void*) { return true; },
            nullptr));
        EXPECT_EQ(readings.present, fault != 4);
        EXPECT_EQ(readings.mapped, fault == 0 ? 1 : 0);
        EXPECT_EQ(readings.installedMissing, 0);
        EXPECT_EQ(readings.originalMissing, fault == 8 ? 1 : 0);
        mapped.currentLesson = 123;
        auto foreign = intent;
        foreign.request.original.transaction[0] ^= 1;
        EXPECT_FALSE(mapUnboundCourseBoundLessons(
            *inspection, *oldReader, *newReader, foreign, scratch, mapped, [](void*) { return true; }, nullptr));
        EXPECT_EQ(mapped.currentLesson, 123);
        readings.mapped = 123;
        EXPECT_FALSE(mapUnboundCourseBoundReadings(
            *inspection, *reviewed, *oldReader, *newReader, foreign, scratch, readings, [](void*) { return true; },
            nullptr));
        EXPECT_EQ(readings.mapped, 123);
        hal.files[ACTIVE_COURSE_PATH].back() ^= 1;
        EXPECT_FALSE(mapUnboundCourseBoundReadings(
            *inspection, *reviewed, *oldReader, *newReader, intent, scratch, readings, [](void*) { return true; },
            nullptr));
        EXPECT_EQ(readings.mapped, 123);
        EXPECT_FALSE(mapUnboundCourseBoundLessons(
            *inspection, *oldReader, *newReader, intent, scratch, mapped, [](void*) { return true; }, nullptr));
        EXPECT_EQ(mapped.currentLesson, 123);
        hal.files[ACTIVE_COURSE_PATH] = bytes;
      }
      permission.permitted = false;
      EXPECT_EQ(inspection->report(request), nullptr);
      permission.permitted = true;
      EXPECT_EQ(inspection->report(request), nullptr);
      ASSERT_TRUE(inspection->inspect(request, original, *parser));
      ASSERT_NE(inspection->report(request), nullptr);
      ASSERT_TRUE(inspection->closeReaders());
    }
    EXPECT_EQ(inspection->report(request), nullptr);
    EXPECT_EQ(reviewed->borrowed(), nullptr);
    EXPECT_EQ(hal.files, files);
  }
}
