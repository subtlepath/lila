#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <iterator>
#include <vector>

#include "lib/Companion/CompanionDictionaryMembersValidation.h"
#include "lib/hal/HalDictionaryArchiveStage.h"
#include "lib/hal/HalDictionaryBundleSource.h"
#include "lib/hal/HalDictionaryCacheStorage.h"

using namespace companion;
namespace {
class HalDictionaryCacheStorageTest : public testing::Test {
 protected:
  std::array<uint8_t, 64> scratch;
  HalDictionaryCacheStorage storage;
  DictionaryCachePublication publication{storage, scratch};
  void SetUp() override { inventory_hal_test::state = {}; }
  static std::vector<uint8_t> fixture(const char* name) {
    std::ifstream file(std::string(COMPANION_FIXTURE_DIR) + "/" + name, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
  }
  ContentManifest candidate() {
    inventory_hal_test::state.files[DICTIONARY_CACHE_CANDIDATE] = fixture("DictionaryBundle-plain.fixture");
    HalFile file;
    Storage.openFileForRead("TEST", DICTIONARY_CACHE_CANDIDATE, file);
    ContentManifest manifest;
    manifest.kind = ContentKind::Dictionary;
    manifest.formatVersion = 1;
    hashInventoryFile(file, scratch, manifest.length, manifest.contentHash);
    return manifest;
  }
};
TEST_F(HalDictionaryCacheStorageTest, PublishesRepairsAndReusesOneVerificationWrapper) {
  const auto manifest = candidate();
  const auto original = inventory_hal_test::state.files.at(DICTIONARY_CACHE_CANDIDATE);
  ASSERT_EQ(publication.publish(manifest), DictionaryCacheResult::Ok);
  const std::string target = publication.publishedPath();
  EXPECT_EQ(inventory_hal_test::state.preparations, 3u);
  EXPECT_EQ(publication.find(manifest), DictionaryCacheResult::Ok);
  inventory_hal_test::state.files[target][0] ^= 1;
  inventory_hal_test::state.files[DICTIONARY_CACHE_CANDIDATE] = original;
  ASSERT_EQ(publication.publish(manifest), DictionaryCacheResult::Ok);
  EXPECT_EQ(inventory_hal_test::state.files.at(target), original);
  EXPECT_EQ(inventory_hal_test::state.preparations, 3u);
}
TEST_F(HalDictionaryCacheStorageTest, ReadOrCloseFailureDoesNotDeleteAnyArchive) {
  const auto manifest = candidate();
  ASSERT_EQ(publication.publish(manifest), DictionaryCacheResult::Ok);
  const std::string target = publication.publishedPath();
  auto& state = inventory_hal_test::state;
  state.files[DICTIONARY_CACHE_CANDIDATE] = state.files.at(target);
  const auto original = state.files;
  state.readErrorPath = target;
  EXPECT_EQ(publication.publish(manifest), DictionaryCacheResult::IoError);
  EXPECT_EQ(state.files, original);
  state.readErrorPath.clear();
  state.failClose = true;
  EXPECT_EQ(publication.publish(manifest), DictionaryCacheResult::IoError);
  EXPECT_EQ(state.files, original);
  state.failClose = false;
  EXPECT_EQ(publication.publish(manifest), DictionaryCacheResult::Ok);
}
TEST_F(HalDictionaryCacheStorageTest, CancelledVerificationAndDirectoryCollisionPreserveCandidate) {
  const auto manifest = candidate();
  const auto original = inventory_hal_test::state.files;
  HalDictionaryCacheStorage cancelled([](void*) { return false; });
  DictionaryCachePublication cancelledPublication(cancelled, scratch);
  EXPECT_EQ(cancelledPublication.publish(manifest), DictionaryCacheResult::IoError);
  EXPECT_EQ(inventory_hal_test::state.files, original);
  inventory_hal_test::state.files.erase(DICTIONARY_CACHE_CANDIDATE);
  inventory_hal_test::state.directories[DICTIONARY_CACHE_CANDIDATE] = {};
  EXPECT_EQ(publication.publish(manifest), DictionaryCacheResult::Corrupt);
  EXPECT_EQ(publication.discardUnpublishedCandidate(), DictionaryCacheResult::IoError);
  EXPECT_TRUE(inventory_hal_test::state.directories.contains(DICTIONARY_CACHE_CANDIDATE));
}
TEST_F(HalDictionaryCacheStorageTest, CompleteMemberAndArchivePipelinePublishesAnImmutableCachePath) {
  auto& files = inventory_hal_test::state.files;
  files["/dictionaries/es/stem.dict"] = {'o', 'n', 'e', 't', 'w', 'o'};
  files["/dictionaries/es/stem.idx"] = fixture("DictionaryIndex-definitions.fixture");
  files["/dictionaries/es/stem.ifo"] = fixture("DictionaryInfo.fixture");
  files["/dictionaries/es/stem.syn"] = fixture("DictionaryIndex-synonyms.fixture");
  const auto original = files;
  HalDictionaryBundleSource source;
  ASSERT_TRUE(source.begin("/dictionaries/es/stem", false, true));
  DictionaryMembersValidation validation(source, scratch);
  DictionaryMembersDetails details;
  ASSERT_TRUE(validation.validate(false, true, details));
  ASSERT_TRUE(source.reopen());
  HalDictionaryArchiveStage stage(scratch);
  DictionaryBundleBuilder builder(stage, scratch);
  uint64_t length;
  ASSERT_TRUE(builder.build(source, false, true, length));
  ContentManifest manifest;
  manifest.kind = ContentKind::Dictionary;
  manifest.formatVersion = 1;
  manifest.length = length;
  manifest.contentHash = stage.contentHash();
  ASSERT_EQ(publication.publish(manifest), DictionaryCacheResult::Ok);
  EXPECT_TRUE(files.contains(publication.publishedPath()));
  EXPECT_FALSE(files.contains(DICTIONARY_CACHE_CANDIDATE));
  for (const auto& [path, bytes] : original) EXPECT_EQ(files.at(path), bytes);
  EXPECT_EQ(inventory_hal_test::state.preparations, 10u);
}
TEST_F(HalDictionaryCacheStorageTest, BooleanExistsCannotHideCacheAndDirectoryIoCannotAuthorizeRemoval) {
  const auto manifest = candidate();
  ASSERT_EQ(publication.publish(manifest), DictionaryCacheResult::Ok);
  const std::string target = publication.publishedPath();
  inventory_hal_test::state.falseExists = true;
  ASSERT_EQ(publication.find(manifest), DictionaryCacheResult::Ok);
  const auto files = inventory_hal_test::state.files;
  inventory_hal_test::state.directoryErrorPath = TRANSFER_DIRECTORY;
  EXPECT_EQ(publication.find(manifest), DictionaryCacheResult::IoError);
  EXPECT_EQ(publication.publishedPath(), nullptr);
  EXPECT_FALSE(storage.remove(target.c_str()));
  EXPECT_EQ(inventory_hal_test::state.files, files);
}
}  // namespace
