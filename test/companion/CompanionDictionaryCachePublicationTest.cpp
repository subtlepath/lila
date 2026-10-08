#include <gtest/gtest.h>
#include <openssl/evp.h>

#include <array>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <vector>

#include "lib/Companion/CompanionDictionaryCachePublication.h"

using namespace companion;
namespace {
class Storage final : public DictionaryCacheStorage {
 public:
  std::map<std::string, std::vector<uint8_t>> files;
  std::set<std::string> directories;
  std::string ioPath;
  unsigned mutations = 0, failAt = 0;
  unsigned inspections = 0;
  bool after = false, failPrepare = false, failFinalRead = false, corruptAfterRename = false;
  bool prepare() override { return !failPrepare; }
  DictionaryCacheCheck inspect(const char* path, const ContentManifest& manifest, std::span<uint8_t>) override {
    if (++inspections == 3 && failFinalRead) return DictionaryCacheCheck::IoError;
    if (path == ioPath) return DictionaryCacheCheck::IoError;
    if (directories.contains(path)) return DictionaryCacheCheck::Collision;
    const auto found = files.find(path);
    if (found == files.end()) return DictionaryCacheCheck::Missing;
    if (found->second.size() != manifest.length) return DictionaryCacheCheck::Corrupt;
    Digest actual{};
    if (EVP_Digest(found->second.data(), found->second.size(), actual.data(), nullptr, EVP_sha256(), nullptr) != 1)
      return DictionaryCacheCheck::IoError;
    return actual == manifest.contentHash ? DictionaryCacheCheck::Valid : DictionaryCacheCheck::Corrupt;
  }
  bool rename(const char* from, const char* to) override {
    const bool fail = ++mutations == failAt;
    if (fail && !after) return false;
    if (!files.contains(from) || files.contains(to) || directories.contains(to)) return false;
    files[to] = std::move(files.at(from));
    if (corruptAfterRename) files[to][0] ^= 1;
    files.erase(from);
    return !fail;
  }
  bool remove(const char* path) override {
    const bool fail = ++mutations == failAt;
    if (fail && !after) return false;
    if (directories.contains(path)) return false;
    files.erase(path);
    return !fail;
  }
};
class CompanionDictionaryCachePublication : public testing::Test {
 protected:
  Storage storage;
  std::array<uint8_t, 64> scratch;
  DictionaryCachePublication publication{storage, scratch};
  ContentManifest manifest;
  std::vector<uint8_t> archive;
  void SetUp() override {
    std::ifstream file(COMPANION_FIXTURE_DIR "/DictionaryBundle-plain.fixture", std::ios::binary);
    archive = {std::istreambuf_iterator<char>(file), {}};
    ASSERT_GT(archive.size(), 22u);
    manifest.kind = ContentKind::Dictionary;
    manifest.formatVersion = 1;
    manifest.length = archive.size();
    ASSERT_EQ(EVP_Digest(archive.data(), archive.size(), manifest.contentHash.data(), nullptr, EVP_sha256(), nullptr),
              1);
    storage.files[DICTIONARY_CACHE_CANDIDATE] = archive;
    storage.files["/dictionaries/es/stem.dict"] = {1, 2, 3};
  }
};
TEST_F(CompanionDictionaryCachePublication, PublishesByHashAndRetriesMissingCandidateWithoutMutations) {
  ASSERT_EQ(publication.publish(manifest), DictionaryCacheResult::Ok);
  ASSERT_NE(publication.publishedPath(), nullptr);
  const std::string target = publication.publishedPath();
  EXPECT_TRUE(target.starts_with("/.crosspoint/companion/dictionary-"));
  EXPECT_TRUE(target.ends_with(".zip"));
  EXPECT_EQ(storage.files.at(target), archive);
  EXPECT_FALSE(storage.files.contains(DICTIONARY_CACHE_CANDIDATE));
  const auto mutations = storage.mutations;
  EXPECT_EQ(publication.publish(manifest), DictionaryCacheResult::Ok);
  EXPECT_EQ(publication.find(manifest), DictionaryCacheResult::Ok);
  EXPECT_EQ(storage.mutations, mutations);
  EXPECT_EQ(storage.files.at("/dictionaries/es/stem.dict"), (std::vector<uint8_t>{1, 2, 3}));
}
TEST_F(CompanionDictionaryCachePublication, EveryMutationFailureBeforeOrAfterCanResume) {
  for (unsigned scenario = 0; scenario < 3; ++scenario) {
    Storage initial = storage;
    DictionaryCachePublication setup(initial, scratch);
    ASSERT_EQ(setup.publish(manifest), DictionaryCacheResult::Ok);
    const std::string target = setup.publishedPath();
    initial.files[DICTIONARY_CACHE_CANDIDATE] = archive;
    if (scenario == 0) initial.files.erase(target);
    if (scenario == 2) initial.files[target][0] ^= 1;
    initial.mutations = 0;
    Storage complete = initial;
    DictionaryCachePublication baseline(complete, scratch);
    ASSERT_EQ(baseline.publish(manifest), DictionaryCacheResult::Ok);
    for (unsigned failure = 1; failure <= complete.mutations; ++failure) {
      for (const bool after : {false, true}) {
        Storage interrupted = initial;
        interrupted.failAt = failure;
        interrupted.after = after;
        DictionaryCachePublication first(interrupted, scratch);
        EXPECT_EQ(first.publish(manifest), DictionaryCacheResult::IoError);
        EXPECT_EQ(first.publishedPath(), nullptr);
        interrupted.failAt = 0;
        DictionaryCachePublication restarted(interrupted, scratch);
        ASSERT_EQ(restarted.publish(manifest), DictionaryCacheResult::Ok) << scenario << failure << after;
        EXPECT_EQ(interrupted.files.at(target), archive);
        EXPECT_FALSE(interrupted.files.contains(DICTIONARY_CACHE_CANDIDATE));
        EXPECT_EQ(interrupted.files.at("/dictionaries/es/stem.dict"), (std::vector<uint8_t>{1, 2, 3}));
      }
    }
  }
}
TEST_F(CompanionDictionaryCachePublication, NeverDeletesCandidateOrCacheOnReadErrorsAndCollisions) {
  ASSERT_EQ(publication.publish(manifest), DictionaryCacheResult::Ok);
  const std::string target = publication.publishedPath();
  storage.files[DICTIONARY_CACHE_CANDIDATE] = archive;
  const auto original = storage.files;
  for (const auto& path : {target, std::string(DICTIONARY_CACHE_CANDIDATE)}) {
    storage.ioPath = path;
    EXPECT_EQ(publication.publish(manifest), DictionaryCacheResult::IoError);
    EXPECT_EQ(storage.files, original);
  }
  storage.ioPath.clear();
  storage.files.erase(target);
  storage.directories.insert(target);
  EXPECT_EQ(publication.publish(manifest), DictionaryCacheResult::Corrupt);
  EXPECT_EQ(storage.files.at(DICTIONARY_CACHE_CANDIDATE), archive);
  storage.directories.clear();
  storage.files[DICTIONARY_CACHE_CANDIDATE][0] ^= 1;
  EXPECT_EQ(publication.publish(manifest), DictionaryCacheResult::Corrupt);
  EXPECT_TRUE(storage.files.contains(DICTIONARY_CACHE_CANDIDATE));
}
TEST_F(CompanionDictionaryCachePublication, InvalidContractsDoNotMutateAndPrivateOrphanCleanupIsExplicit) {
  for (unsigned mutation = 0; mutation < 5; ++mutation) {
    auto invalid = manifest;
    if (mutation == 0) invalid.kind = ContentKind::Course;
    if (mutation == 1) invalid.formatVersion = 2;
    if (mutation == 2) invalid.logicalIdentity[0] = 1;
    if (mutation == 3) invalid.length = 21;
    if (mutation == 4) invalid.contentHash.fill(0);
    EXPECT_EQ(publication.publish(invalid), DictionaryCacheResult::Invalid);
    EXPECT_EQ(storage.mutations, 0u);
    EXPECT_EQ(publication.publishedPath(), nullptr);
  }
  EXPECT_EQ(publication.discardUnpublishedCandidate(), DictionaryCacheResult::Ok);
  EXPECT_FALSE(storage.files.contains(DICTIONARY_CACHE_CANDIDATE));
  EXPECT_TRUE(storage.files.contains("/dictionaries/es/stem.dict"));
  EXPECT_EQ(publication.find(manifest), DictionaryCacheResult::Missing);
}
TEST_F(CompanionDictionaryCachePublication, FinalReadFailureAndCorruptionNeverExposePublishedPath) {
  for (const bool corruption : {false, true}) {
    Storage interrupted = storage;
    interrupted.failFinalRead = !corruption;
    interrupted.corruptAfterRename = corruption;
    DictionaryCachePublication first(interrupted, scratch);
    EXPECT_EQ(first.publish(manifest), corruption ? DictionaryCacheResult::Corrupt : DictionaryCacheResult::IoError);
    EXPECT_EQ(first.publishedPath(), nullptr);
    interrupted.failFinalRead = interrupted.corruptAfterRename = false;
    if (corruption) interrupted.files[DICTIONARY_CACHE_CANDIDATE] = archive;
    DictionaryCachePublication resumed(interrupted, scratch);
    ASSERT_EQ(resumed.publish(manifest), DictionaryCacheResult::Ok);
    EXPECT_EQ(interrupted.files.at(resumed.publishedPath()), archive);
    EXPECT_EQ(interrupted.files.at("/dictionaries/es/stem.dict"), (std::vector<uint8_t>{1, 2, 3}));
  }
}
}  // namespace
