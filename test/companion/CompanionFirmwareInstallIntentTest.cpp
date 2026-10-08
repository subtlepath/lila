#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "lib/Companion/CompanionFirmwareInstallAdmission.h"

namespace {
class Storage final : public companion::TransferStorage {
 public:
  std::map<std::string, std::vector<uint8_t>> files;
  int fail = 0, mutations = 0;
  bool after = false;
  bool prepare() override { return true; }
  companion::FileStatus stat(const char* path, uint64_t& size) override {
    if (!files.contains(path)) return companion::FileStatus::Missing;
    size = files[path].size();
    return companion::FileStatus::Present;
  }
  bool read(const char* path, uint64_t offset, std::span<uint8_t> bytes) override {
    if (!files.contains(path) || offset > files[path].size() || bytes.size() > files[path].size() - offset)
      return false;
    std::memcpy(bytes.data(), files[path].data() + offset, bytes.size());
    return true;
  }
  bool write(const char* path, uint64_t offset, std::span<const uint8_t> bytes, bool truncate) override {
    if (offset || !truncate) return false;
    const bool failed = ++mutations == fail;
    if (failed && !after) return false;
    files[path] = {bytes.begin(), bytes.end()};
    return !failed;
  }
  bool rename(const char* from, const char* to) override {
    const bool failed = ++mutations == fail;
    if (failed && !after) return false;
    if (!files.contains(from) || files.contains(to)) return false;
    files[to] = std::move(files[from]);
    files.erase(from);
    return !failed;
  }
  bool resize(const char*, uint64_t) override { return false; }
  bool remove(const char* path) override {
    const bool failed = ++mutations == fail;
    if (failed && !after) return false;
    if (!files.contains(path)) return false;
    files.erase(path);
    return !failed;
  }
  bool verify(const char* path, uint64_t length, const companion::Digest& hash, std::span<uint8_t>) override {
    return files.contains(path) && files[path].size() == length && !files[path].empty() && files[path][0] == hash[0];
  }
};
companion::FirmwareInstallAuthorization request() {
  companion::FirmwareInstallAuthorization result;
  result.owner.fill(4);
  result.request.generation.fill(1);
  result.request.transaction.fill(2);
  result.request.hash.fill(3);
  result.request.length = 65536;
  result.request.partitionBytes = 0x900000;
  result.request.chip = 5;
  return result;
}
}  // namespace
TEST(CompanionFirmwareInstallIntent, DurableIdempotentAndConflictingConsentPreserved) {
  Storage storage;
  std::array<uint8_t, companion::FIRMWARE_INSTALL_INTENT_SIZE> scratch;
  companion::FirmwareInstallIntent intent(storage, scratch);
  const auto expected = request();
  EXPECT_EQ(intent.persist(expected), companion::FirmwareInstallIntentResult::Ok);
  const auto files = storage.files;
  EXPECT_EQ(intent.persist(expected), companion::FirmwareInstallIntentResult::Ok);
  EXPECT_EQ(storage.mutations, 2);
  auto other = expected;
  other.request.transaction[0]++;
  EXPECT_EQ(intent.persist(other), companion::FirmwareInstallIntentResult::Conflict);
  EXPECT_EQ(storage.files, files);
  companion::FirmwareInstallAuthorization loaded;
  EXPECT_EQ(intent.load(loaded), companion::FirmwareInstallIntentResult::Ok);
  EXPECT_EQ(loaded, expected);
}
TEST(CompanionFirmwareInstallIntent, EveryInterruptedMutationResumesExactConsent) {
  for (int failure = 1; failure <= 2; ++failure) {
    for (bool after : {false, true}) {
      Storage storage;
      storage.fail = failure;
      storage.after = after;
      std::array<uint8_t, companion::FIRMWARE_INSTALL_INTENT_SIZE> scratch;
      companion::FirmwareInstallIntent intent(storage, scratch);
      EXPECT_EQ(intent.persist(request()), companion::FirmwareInstallIntentResult::IoError);
      storage.fail = 0;
      companion::FirmwareInstallIntent restarted(storage, scratch);
      EXPECT_EQ(restarted.persist(request()), companion::FirmwareInstallIntentResult::Ok);
      companion::FirmwareInstallAuthorization loaded;
      EXPECT_EQ(restarted.load(loaded), companion::FirmwareInstallIntentResult::Ok);
      EXPECT_EQ(loaded, request());
    }
  }
}
TEST(CompanionFirmwareInstallIntent, ForeignOwnersAndTornRecordsPreserveEvidence) {
  using namespace companion;
  for (const char* path : {FIRMWARE_INSTALL_INTENT_PATH, FIRMWARE_INSTALL_INTENT_STAGE}) {
    Storage storage;
    storage.files[path] = {1, 2, 3};
    auto files = storage.files;
    std::array<uint8_t, FIRMWARE_INSTALL_INTENT_SIZE> scratch;
    FirmwareInstallIntent intent(storage, scratch);
    EXPECT_EQ(intent.persist(request()), FirmwareInstallIntentResult::Corrupt);
    EXPECT_EQ(storage.files, files);
    EXPECT_EQ(storage.mutations, 0);
  }
  Storage storage;
  std::array<uint8_t, FIRMWARE_INSTALL_INTENT_SIZE> scratch;
  FirmwareInstallIntent intent(storage, scratch);
  ASSERT_EQ(intent.persist(request()), FirmwareInstallIntentResult::Ok);
  auto files = storage.files;
  auto other = request();
  other.owner[0] ^= 1;
  EXPECT_EQ(intent.persist(other), FirmwareInstallIntentResult::Conflict);
  EXPECT_EQ(storage.files, files);
  storage.files[FIRMWARE_INSTALL_INTENT_PATH][105] ^= 1;
  auto loaded = request();
  EXPECT_EQ(intent.load(loaded), FirmwareInstallIntentResult::Corrupt);
  EXPECT_EQ(loaded, request());
}
TEST(CompanionFirmwareInstallIntent, RehashesBeforeIntentAndRequiresActualBootDigest) {
  using namespace companion;
  Storage storage;
  std::array<uint8_t, FIRMWARE_INSTALL_INTENT_SIZE> scratch;
  const auto authorization = request();
  FirmwareReaderInfo fresh;
  fresh.generation = authorization.request.generation;
  fresh.runningBuild.fill(8);
  fresh.chip = 5;
  fresh.battery = 60;
  fresh.partitionBytes = authorization.request.partitionBytes;
  TransferState state;
  state.transaction = authorization.request.transaction;
  state.storageGeneration = fresh.generation;
  state.owner = authorization.owner;
  state.contentHash = authorization.request.hash;
  state.length = state.durableOffset = authorization.request.length;
  state.phase = TransferPhase::Committed;
  ContentManifest manifest;
  manifest.kind = ContentKind::Firmware;
  manifest.formatVersion = 1;
  manifest.contentHash = state.contentHash;
  manifest.length = state.length;
  EXPECT_EQ(
      admitFirmwareInstallation(storage, authorization, fresh, state, manifest, FIRMWARE_STAGE_DESTINATION, scratch),
      FirmwareInstallIntentResult::Corrupt);
  EXPECT_TRUE(storage.files.empty());
  storage.files[FIRMWARE_STAGE_DESTINATION] = std::vector<uint8_t>(state.length, 3);
  auto incompatible = fresh;
  incompatible.battery = 29;
  EXPECT_EQ(admitFirmwareInstallation(storage, authorization, incompatible, state, manifest, FIRMWARE_STAGE_DESTINATION,
                                      scratch),
            FirmwareInstallIntentResult::Invalid);
  EXPECT_EQ(storage.mutations, 0);
  ASSERT_EQ(
      admitFirmwareInstallation(storage, authorization, fresh, state, manifest, FIRMWARE_STAGE_DESTINATION, scratch),
      FirmwareInstallIntentResult::Ok);
  EXPECT_FALSE(firmwareInstallationBootVerified(authorization, fresh));
  fresh.runningBuild = authorization.request.hash;
  EXPECT_TRUE(firmwareInstallationBootVerified(authorization, fresh));
  fresh.generation[0] ^= 1;
  EXPECT_FALSE(firmwareInstallationBootVerified(authorization, fresh));
  const auto files = storage.files;
  storage.files[FIRMWARE_STAGE_DESTINATION][0] ^= 1;
  EXPECT_EQ(
      admitFirmwareInstallation(storage, authorization, fresh, state, manifest, FIRMWARE_STAGE_DESTINATION, scratch),
      FirmwareInstallIntentResult::Invalid);
  fresh.generation = authorization.request.generation;
  EXPECT_EQ(
      admitFirmwareInstallation(storage, authorization, fresh, state, manifest, FIRMWARE_STAGE_DESTINATION, scratch),
      FirmwareInstallIntentResult::Corrupt);
  EXPECT_EQ(storage.files[FIRMWARE_INSTALL_INTENT_PATH], files.at(FIRMWARE_INSTALL_INTENT_PATH));
}
TEST(CompanionFirmwareInstallIntent, RetiresOnlyVerifiedBootAndPreservesAmbiguousStages) {
  using namespace companion;
  for (int failure : {0, 3}) {
    for (bool after : {false, true}) {
      Storage storage;
      std::array<uint8_t, FIRMWARE_INSTALL_INTENT_SIZE> scratch;
      FirmwareInstallIntent intent(storage, scratch);
      ASSERT_EQ(intent.persist(request()), FirmwareInstallIntentResult::Ok);
      FirmwareReaderInfo running;
      running.generation = request().request.generation;
      running.runningBuild.fill(8);
      running.battery = 60;
      running.chip = 5;
      running.partitionBytes = 0x900000;
      const auto files = storage.files;
      EXPECT_EQ(intent.retireVerified(running), FirmwareInstallIntentResult::Conflict);
      EXPECT_EQ(storage.files, files);
      running.runningBuild = request().request.hash;
      storage.files[FIRMWARE_INSTALL_INTENT_STAGE] = {1};
      EXPECT_EQ(intent.retireVerified(running), FirmwareInstallIntentResult::Conflict);
      storage.files.erase(FIRMWARE_INSTALL_INTENT_STAGE);
      storage.fail = failure;
      storage.after = after;
      EXPECT_EQ(intent.retireVerified(running),
                failure ? FirmwareInstallIntentResult::IoError : FirmwareInstallIntentResult::Ok);
      storage.fail = 0;
      FirmwareInstallIntent restarted(storage, scratch);
      EXPECT_EQ(restarted.retireVerified(running),
                failure && !after ? FirmwareInstallIntentResult::Ok : FirmwareInstallIntentResult::Missing);
      EXPECT_TRUE(storage.files.empty());
    }
  }
}
