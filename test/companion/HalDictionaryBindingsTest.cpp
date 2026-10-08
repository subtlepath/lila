#include <gtest/gtest.h>
#include <openssl/evp.h>

#include <array>
#include <fstream>
#include <iterator>

#include "lib/hal/HalDictionaryBindings.h"
#include "lib/hal/HalDictionaryCacheStorage.h"
using namespace companion;
namespace {
class HalDictionaryBindingsTest : public testing::Test {
 protected:
  std::array<uint8_t, 256> scratch;
  HalDictionaryCacheStorage cache;
  HalDictionaryBindings bindings{cache, scratch};
  void SetUp() override { inventory_hal_test::state = {}; }
  static std::vector<uint8_t> fixture(const char* name) {
    std::ifstream file(std::string(COMPANION_FIXTURE_DIR) + "/" + name, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
  }
  ContentManifest archive(bool compressed, bool comment) {
    auto bytes = fixture(compressed ? "DictionaryBundle-dictzip.fixture" : "DictionaryBundle-plain.fixture");
    if (comment) {
      bytes[bytes.size() - 2] = 3;
      bytes.insert(bytes.end(), {'z', 'i', 'p'});
    }
    auto& state = inventory_hal_test::state;
    state.files[DICTIONARY_CACHE_CANDIDATE] = bytes;
    ContentManifest manifest;
    manifest.kind = ContentKind::Dictionary;
    manifest.formatVersion = 1;
    manifest.length = bytes.size();
    EVP_Digest(bytes.data(), bytes.size(), manifest.contentHash.data(), nullptr, EVP_sha256(), nullptr);
    DictionaryCachePublication publication(cache, scratch);
    EXPECT_EQ(publication.publish(manifest), DictionaryCacheResult::Ok);
    return manifest;
  }
  DictionaryArchiveBinding value(bool compressed = false) {
    return {archive(compressed, false), archive(compressed, true)};
  }
  static std::string target(const char* path = "/dictionaries/es/stem") {
    Digest hash{};
    EVP_Digest(path, strlen(path), hash.data(), nullptr, EVP_sha256(), nullptr);
    std::string result = "/.crosspoint/companion/dictionary-binding-";
    result.reserve(110);
    constexpr char HEX[] = "0123456789abcdef";
    for (auto byte : hash) {
      result += HEX[byte >> 4];
      result += HEX[byte & 15];
    }
    return result;
  }
};
TEST_F(HalDictionaryBindingsTest, PersistedBindingSurvivesReconstructionAndRetainsOneHandle) {
  const auto expected = value();
  DictionaryArchiveBinding output;
  EXPECT_EQ(bindings.read("/dictionaries/es/stem", output), DictionaryBindingResult::Missing);
  ASSERT_TRUE(bindings.install("/dictionaries/es/stem", expected));
  ASSERT_TRUE(bindings.finalizeInstallation("/dictionaries/es/stem", expected));
  const auto wrappers = inventory_hal_test::state.preparations;
  for (unsigned read = 0; read < 5; ++read) {
    ASSERT_EQ(bindings.read("/dictionaries/es/stem", output), DictionaryBindingResult::Found);
    EXPECT_EQ(output, expected);
    EXPECT_EQ(inventory_hal_test::state.preparations, wrappers);
  }
  HalDictionaryBindings rebooted(cache, scratch);
  ASSERT_EQ(rebooted.read("/dictionaries/es/stem", output), DictionaryBindingResult::Found);
  EXPECT_EQ(output, expected);
  EXPECT_EQ(rebooted.read("/dictionaries/fr/stem", output), DictionaryBindingResult::Missing);
  EXPECT_EQ(output, expected);
}
TEST_F(HalDictionaryBindingsTest, ReplacementKeepsBackupAndBlocksNormalReadsUntilFinalized) {
  const auto old = value(), next = value(true);
  ASSERT_TRUE(bindings.install("/dictionaries/es/stem", old));
  ASSERT_TRUE(bindings.finalizeInstallation("/dictionaries/es/stem", old));
  const auto oldBytes = inventory_hal_test::state.files.at(target());
  ASSERT_TRUE(bindings.install("/dictionaries/es/stem", next));
  EXPECT_EQ(inventory_hal_test::state.files.at(target() + ".bak"), oldBytes);
  auto output = old;
  EXPECT_EQ(bindings.read("/dictionaries/es/stem", output), DictionaryBindingResult::Error);
  EXPECT_EQ(output, old);
  EXPECT_FALSE(bindings.finalizeInstallation("/dictionaries/es/stem", old));
  ASSERT_TRUE(bindings.install("/dictionaries/es/stem", next));
  ASSERT_TRUE(bindings.finalizeInstallation("/dictionaries/es/stem", next));
  ASSERT_EQ(bindings.read("/dictionaries/es/stem", output), DictionaryBindingResult::Found);
  EXPECT_EQ(output, next);
}
TEST_F(HalDictionaryBindingsTest, EveryRenameFailureBeforeOrAfterEffectsRetriesWithoutLosingOldBinding) {
  for (bool after : {false, true}) {
    for (unsigned mutation = 1; mutation <= 2; ++mutation) {
      SetUp();
      const auto old = value(), next = value(true);
      ASSERT_TRUE(bindings.install("/dictionaries/es/stem", old));
      ASSERT_TRUE(bindings.finalizeInstallation("/dictionaries/es/stem", old));
      auto& state = inventory_hal_test::state;
      const auto oldBytes = state.files.at(target());
      if (after)
        state.failRenameAfter = state.renames + mutation;
      else
        state.failRename = state.renames + mutation;
      EXPECT_FALSE(bindings.install("/dictionaries/es/stem", next));
      EXPECT_TRUE((state.files.contains(target()) && state.files.at(target()) == oldBytes) ||
                  (state.files.contains(target() + ".bak") && state.files.at(target() + ".bak") == oldBytes));
      auto output = old;
      EXPECT_EQ(bindings.read("/dictionaries/es/stem", output), DictionaryBindingResult::Error);
      state.failRename = state.failRenameAfter = 0;
      ASSERT_TRUE(bindings.install("/dictionaries/es/stem", next));
      ASSERT_TRUE(bindings.finalizeInstallation("/dictionaries/es/stem", next));
      ASSERT_EQ(bindings.read("/dictionaries/es/stem", output), DictionaryBindingResult::Found);
      EXPECT_EQ(output, next);
    }
  }
}
TEST_F(HalDictionaryBindingsTest, CleanupFailureBeforeOrAfterEffectsCanBeRetried) {
  for (bool after : {false, true}) {
    SetUp();
    const auto old = value(), next = value(true);
    ASSERT_TRUE(bindings.install("/dictionaries/es/stem", old));
    ASSERT_TRUE(bindings.finalizeInstallation("/dictionaries/es/stem", old));
    ASSERT_TRUE(bindings.install("/dictionaries/es/stem", next));
    if (after)
      inventory_hal_test::state.failRemoveAfter = true;
    else
      inventory_hal_test::state.failRemove = true;
    EXPECT_FALSE(bindings.finalizeInstallation("/dictionaries/es/stem", next));
    inventory_hal_test::state.failRemove = inventory_hal_test::state.failRemoveAfter = false;
    ASSERT_TRUE(bindings.finalizeInstallation("/dictionaries/es/stem", next));
    DictionaryArchiveBinding output;
    EXPECT_EQ(bindings.read("/dictionaries/es/stem", output), DictionaryBindingResult::Found);
    EXPECT_EQ(output, next);
  }
}
TEST_F(HalDictionaryBindingsTest, CorruptReadCloseAndWriteFailuresPreserveExistingBinding) {
  for (unsigned failure = 0; failure < 9; ++failure) {
    SetUp();
    const auto old = value(), next = value(true);
    ASSERT_TRUE(bindings.install("/dictionaries/es/stem", old));
    ASSERT_TRUE(bindings.finalizeInstallation("/dictionaries/es/stem", old));
    auto& state = inventory_hal_test::state;
    const auto oldBytes = state.files.at(target());
    if (failure == 0) state.failWrite = true;
    if (failure == 1) state.failSync = true;
    if (failure == 2) state.failTruncate = true;
    if (failure == 3) state.corruptWrite = true;
    if (failure == 4) state.failClose = true;
    if (failure == 5) state.readErrorPath = target();
    if (failure == 6) state.failOpen = true;
    if (failure == 7) state.failClosePath = target() + ".tmp";
    if (failure == 8) state.readErrorPath = target() + ".tmp";
    EXPECT_FALSE(bindings.install("/dictionaries/es/stem", next)) << failure;
    EXPECT_EQ(state.files.at(target()), oldBytes);
    state.failWrite = state.failSync = state.failTruncate = state.corruptWrite = state.failClose = state.failOpen =
        false;
    state.readErrorPath.clear();
    state.failClosePath.clear();
    ASSERT_TRUE(bindings.install("/dictionaries/es/stem", next));
    ASSERT_TRUE(bindings.finalizeInstallation("/dictionaries/es/stem", next));
  }
}
TEST_F(HalDictionaryBindingsTest, WrongPathCorruptRecordMissingArchivesAndInvalidArgumentsFailClosed) {
  const auto expected = value();
  ASSERT_TRUE(bindings.install("/dictionaries/es/stem", expected));
  ASSERT_TRUE(bindings.finalizeInstallation("/dictionaries/es/stem", expected));
  auto& state = inventory_hal_test::state;
  auto output = expected;
  state.files[target("/dictionaries/fr/stem")] = state.files.at(target());
  EXPECT_EQ(bindings.read("/dictionaries/fr/stem", output), DictionaryBindingResult::Error);
  EXPECT_EQ(output, expected);
  state.files[target()][10] ^= 1;
  EXPECT_EQ(bindings.read("/dictionaries/es/stem", output), DictionaryBindingResult::Error);
  EXPECT_EQ(output, expected);
  EXPECT_FALSE(bindings.install("/dictionaries/es/stem", expected));
  EXPECT_FALSE(bindings.install("/dictionaries/../es", expected));
  EXPECT_EQ(bindings.read(nullptr, output), DictionaryBindingResult::Error);
  state.files.clear();
  EXPECT_FALSE(bindings.install("/dictionaries/es/stem", expected));
  EXPECT_TRUE(state.files.empty());
}
TEST_F(HalDictionaryBindingsTest, ReadCloseFailurePreservesOutputAndRetrySucceeds) {
  const auto expected = value();
  ASSERT_TRUE(bindings.install("/dictionaries/es/stem", expected));
  ASSERT_TRUE(bindings.finalizeInstallation("/dictionaries/es/stem", expected));
  auto output = expected;
  output.original.length = 99;
  inventory_hal_test::state.failClosePath = target();
  EXPECT_EQ(bindings.read("/dictionaries/es/stem", output), DictionaryBindingResult::Error);
  EXPECT_EQ(output.original.length, 99u);
  inventory_hal_test::state.failClosePath.clear();
  ASSERT_EQ(bindings.read("/dictionaries/es/stem", output), DictionaryBindingResult::Found);
  EXPECT_EQ(output, expected);
}
TEST_F(HalDictionaryBindingsTest, VerifiedReplacementCanFinishWithDamagedOwnedBackup) {
  const auto old = value(), next = value(true);
  ASSERT_TRUE(bindings.install("/dictionaries/es/stem", old));
  ASSERT_TRUE(bindings.finalizeInstallation("/dictionaries/es/stem", old));
  ASSERT_TRUE(bindings.install("/dictionaries/es/stem", next));
  inventory_hal_test::state.files[target() + ".bak"][0] ^= 1;
  DictionaryArchiveBinding output;
  EXPECT_EQ(bindings.read("/dictionaries/es/stem", output), DictionaryBindingResult::Error);
  ASSERT_TRUE(bindings.install("/dictionaries/es/stem", next));
  ASSERT_TRUE(bindings.finalizeInstallation("/dictionaries/es/stem", next));
  EXPECT_FALSE(inventory_hal_test::state.files.contains(target() + ".bak"));
  ASSERT_EQ(bindings.read("/dictionaries/es/stem", output), DictionaryBindingResult::Found);
  EXPECT_EQ(output, next);
}
TEST_F(HalDictionaryBindingsTest, BooleanExistsCannotHideBindingAndDirectoryIoPreservesOutput) {
  const auto expected = value();
  ASSERT_TRUE(bindings.install("/dictionaries/es/stem", expected));
  ASSERT_TRUE(bindings.finalizeInstallation("/dictionaries/es/stem", expected));
  inventory_hal_test::state.falseExists = true;
  DictionaryArchiveBinding output;
  ASSERT_EQ(bindings.read("/dictionaries/es/stem", output), DictionaryBindingResult::Found);
  EXPECT_EQ(output, expected);
  const auto files = inventory_hal_test::state.files;
  inventory_hal_test::state.directoryErrorPath = TRANSFER_DIRECTORY;
  output.original.length = 99;
  EXPECT_EQ(bindings.read("/dictionaries/es/stem", output), DictionaryBindingResult::Error);
  EXPECT_EQ(output.original.length, 99u);
  EXPECT_FALSE(bindings.install("/dictionaries/es/stem", expected));
  EXPECT_FALSE(bindings.finalizeInstallation("/dictionaries/es/stem", expected));
  EXPECT_EQ(inventory_hal_test::state.files, files);
}
}  // namespace
