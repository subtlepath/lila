#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <iterator>

#include "lib/Memory/Memory.h"
#include "lib/hal/HalDictionaryBindings.h"
#include "lib/hal/HalDictionaryFinalizedContentVerification.h"
#include "lib/hal/HalInventoryDictionaryResolver.h"
#include "lib/hal/HalReaderPreferenceDictionaryProof.h"
using namespace companion;
namespace {
struct Delegate : InventoryFileResolver {
  InventoryFileDecision resolve(const char* path, HalFile& file, ContentManifest&) override {
    if (std::string(path) == "/.crosspoint") return InventoryFileDecision::Skip;
    return file.isDirectory() ? InventoryFileDecision::Include : InventoryFileDecision::Skip;
  }
};
struct Bindings : DictionaryArchiveBindings {
  DictionaryBindingResult result = DictionaryBindingResult::Missing;
  DictionaryArchiveBinding binding;
  std::string base;
  DictionaryBindingResult read(const char* path, DictionaryArchiveBinding& output) override {
    base = path;
    if (result == DictionaryBindingResult::Found) output = binding;
    return result;
  }
};
struct Paths : InventoryPathSink {
  std::vector<std::pair<ContentManifest, std::string>> entries;
  Paths() { entries.reserve(8); }
  bool record(const ContentManifest& metadata, const char* path) override {
    entries.emplace_back(metadata, path);
    return true;
  }
};
class HalInventoryDictionaryResolverTest : public testing::Test {
 protected:
  Delegate delegate;
  Bindings bindings;
  std::array<uint8_t, 64> scratch;
  static std::vector<uint8_t> fixture(const char* name) {
    std::ifstream file(std::string(COMPANION_FIXTURE_DIR) + "/" + name, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
  }
  void SetUp() override {
    inventory_hal_test::state = {};
    auto& state = inventory_hal_test::state;
    state.directories["/"] = {{"dictionaries", true}, {".crosspoint", true}};
    state.directories["/dictionaries"] = {{"es", true}};
    state.directories["/dictionaries/es"] = {{"stem.idx"}, {"stem.ifo"}, {"stem.dict"}, {"stem.syn"}};
    state.files["/dictionaries/es/stem.dict"] = {'o', 'n', 'e', 't', 'w', 'o'};
    state.files["/dictionaries/es/stem.idx"] = fixture("DictionaryIndex-definitions.fixture");
    const std::string info =
        "StarDict's dict ifo file\nversion=3.0.0\nbookname=Canonical "
        "dictionary\nwordcount=2\nidxfilesize=24\nsynwordcount=1\n";
    state.files["/dictionaries/es/stem.ifo"] = {info.begin(), info.end()};
    state.files["/dictionaries/es/stem.syn"] = fixture("DictionaryIndex-synonyms.fixture");
  }
};
TEST_F(HalInventoryDictionaryResolverTest, FullScanExportsLegacyBundleAndReusesPipelineHandles) {
  HalInventoryDictionaryResolver resolver(delegate, bindings, scratch);
  ASSERT_TRUE(resolver.prepareAfterRecovery());
  HalInventoryDirectoryWalker walker;
  Paths paths;
  HalInventoryFileSource source(walker, resolver, paths, scratch);
  unsigned wrappers = 0;
  for (unsigned scan = 0; scan < 3; ++scan) {
    ASSERT_TRUE(source.begin());
    ContentManifest manifest;
    ASSERT_EQ(source.next(manifest), InventorySourceResult::Entry);
    EXPECT_EQ(manifest.kind, ContentKind::Dictionary);
    EXPECT_EQ(bindings.base, "/dictionaries/es/stem");
    EXPECT_EQ(inventory_hal_test::state.files.at(paths.entries.back().second),
              fixture("DictionaryBundle-plain.fixture"));
    EXPECT_FALSE(inventory_hal_test::state.files.contains(DICTIONARY_CACHE_CANDIDATE));
    EXPECT_EQ(source.next(manifest), InventorySourceResult::End);
    ASSERT_TRUE(source.closeScan());
    if (scan == 0) wrappers = inventory_hal_test::state.preparations;
    EXPECT_EQ(inventory_hal_test::state.preparations, wrappers);
  }
  EXPECT_EQ(paths.entries.size(), 3u);
  EXPECT_EQ(paths.entries.front().first, paths.entries.back().first);
}
TEST_F(HalInventoryDictionaryResolverTest, RetainedOriginalArchiveKeepsExactTransferredIdentity) {
  HalInventoryDictionaryResolver resolver(delegate, bindings, scratch);
  ASSERT_TRUE(resolver.prepareAfterRecovery());
  ContentManifest canonical;
  const char* path = nullptr;
  ASSERT_EQ(resolver.resolveBundle("/dictionaries/es", canonical, path), InventoryFileDecision::Include);
  auto original = fixture("DictionaryBundle-plain.fixture");
  original[original.size() - 2] = 3;
  original.insert(original.end(), {'z', 'i', 'p'});
  auto& state = inventory_hal_test::state;
  state.files[DICTIONARY_CACHE_CANDIDATE] = original;
  HalFile file;
  ASSERT_TRUE(Storage.openFileForRead("TEST", DICTIONARY_CACHE_CANDIDATE, file));
  ContentManifest transferred = canonical;
  ASSERT_TRUE(hashInventoryFile(file, scratch, transferred.length, transferred.contentHash));
  ASSERT_TRUE(file.close());
  HalDictionaryCacheStorage storage;
  DictionaryCachePublication publication(storage, scratch);
  ASSERT_EQ(publication.publish(transferred), DictionaryCacheResult::Ok);
  const std::string originalPath = publication.publishedPath();
  bindings.result = DictionaryBindingResult::Found;
  bindings.binding = {canonical, transferred};
  ContentManifest selected;
  ASSERT_EQ(resolver.resolveBundle("/dictionaries/es", selected, path), InventoryFileDecision::Include);
  EXPECT_EQ(selected, transferred);
  EXPECT_STREQ(path, originalPath.c_str());
  EXPECT_TRUE(resolver.verifyHashed(path, transferred));
  EXPECT_FALSE(resolver.verifyHashed(path, canonical));
  state.files.erase(originalPath);
  selected.length = 99;
  path = "untouched";
  EXPECT_EQ(resolver.resolveBundle("/dictionaries/es", selected, path), InventoryFileDecision::Error);
  EXPECT_EQ(selected.length, 99u);
  EXPECT_STREQ(path, "untouched");
}
TEST_F(HalInventoryDictionaryResolverTest, InvalidMembersBindingsAndIoNeverExposeAnArchive) {
  for (unsigned failure = 0; failure < 5; ++failure) {
    SetUp();
    bindings.result = DictionaryBindingResult::Missing;
    HalInventoryDictionaryResolver resolver(delegate, bindings, scratch);
    ASSERT_TRUE(resolver.prepareAfterRecovery());
    if (failure == 0) inventory_hal_test::state.files["/dictionaries/es/stem.idx"][0] = 0;
    if (failure == 1) inventory_hal_test::state.readErrorPath = "/dictionaries/es/stem.ifo";
    if (failure == 2) inventory_hal_test::state.corruptWrite = true;
    if (failure == 3) bindings.result = DictionaryBindingResult::Error;
    if (failure == 4) {
      bindings.result = DictionaryBindingResult::Found;
      bindings.binding = {};
    }
    ContentManifest manifest;
    manifest.length = 99;
    const char* path = "untouched";
    EXPECT_EQ(resolver.resolveBundle("/dictionaries/es", manifest, path), InventoryFileDecision::Error) << failure;
    EXPECT_EQ(manifest.length, 99u);
    EXPECT_STREQ(path, "untouched");
    EXPECT_EQ(inventory_hal_test::state.files.at("/dictionaries/es/stem.dict"),
              (std::vector<uint8_t>{'o', 'n', 'e', 't', 'w', 'o'}));
  }
}
TEST_F(HalInventoryDictionaryResolverTest, RecoveryIsRequiredAndPrivateCandidateIsExplicitlyDiscarded) {
  HalInventoryDictionaryResolver resolver(delegate, bindings, scratch);
  ContentManifest manifest;
  const char* path = nullptr;
  EXPECT_EQ(resolver.resolveBundle("/dictionaries/es", manifest, path), InventoryFileDecision::Error);
  inventory_hal_test::state.files[DICTIONARY_CACHE_CANDIDATE] = {1, 2, 3};
  ASSERT_TRUE(resolver.prepareAfterRecovery());
  EXPECT_FALSE(inventory_hal_test::state.files.contains(DICTIONARY_CACHE_CANDIDATE));
  ASSERT_EQ(resolver.resolveBundle("/dictionaries/es", manifest, path), InventoryFileDecision::Include);
}
TEST_F(HalInventoryDictionaryResolverTest, CompressedDictionaryUsesBorrowedDecoderAndWindow) {
  auto& state = inventory_hal_test::state;
  state.files.erase("/dictionaries/es/stem.dict");
  state.files["/dictionaries/es/stem.dict.dz"] = fixture("dictzip-chunks.dict.dz");
  state.directories["/dictionaries/es"][2].name = "stem.dict.dz";
  std::array<uint8_t, 32768> window;
  tinfl_decompressor decoder{};
  HalInventoryDictionaryResolver resolver(delegate, bindings, scratch, &decoder, window);
  ASSERT_TRUE(resolver.prepareAfterRecovery());
  ContentManifest manifest;
  const char* path = nullptr;
  ASSERT_EQ(resolver.resolveBundle("/dictionaries/es", manifest, path), InventoryFileDecision::Include);
  EXPECT_EQ(state.files.at(path), fixture("DictionaryBundle-dictzip.fixture"));
  std::array<uint8_t, 256> verificationScratch{};
  HalDictionaryCacheStorage cache;
  HalDictionaryBindings durable(cache, verificationScratch);
  ASSERT_TRUE(durable.install("/dictionaries/es/stem", {manifest, manifest}));
  ASSERT_TRUE(durable.finalizeInstallation("/dictionaries/es/stem", {manifest, manifest}));
  TransferState committed;
  committed.phase = TransferPhase::Committed;
  committed.owner[0] = 1;
  committed.transaction[0] = 2;
  committed.storageGeneration[0] = 3;
  committed.contentHash = manifest.contentHash;
  committed.length = committed.durableOffset = manifest.length;
  auto verifier = makeUniqueNoThrow<HalDictionaryFinalizedContentVerification>(verificationScratch);
  ASSERT_TRUE(verifier);
  const auto saved = state.files;
  EXPECT_TRUE(verifier->verify("/dictionaries/es/stem", manifest, committed));
  EXPECT_EQ(state.files, saved);
  state.files.at("/dictionaries/es/stem.dict.dz")[0] ^= 1;
  EXPECT_FALSE(verifier->verify("/dictionaries/es/stem", manifest, committed));
}
TEST_F(HalInventoryDictionaryResolverTest, CancellationCanRetryAndFolderBoundariesAreExplicit) {
  bool allowed = true;
  auto progress = [](void* context) { return *static_cast<bool*>(context); };
  HalInventoryDictionaryResolver resolver(delegate, bindings, scratch, nullptr, {}, progress, &allowed);
  ASSERT_TRUE(resolver.prepareAfterRecovery());
  allowed = false;
  ContentManifest manifest;
  manifest.length = 99;
  const char* path = "untouched";
  EXPECT_EQ(resolver.resolveBundle("/dictionaries/es", manifest, path), InventoryFileDecision::Error);
  EXPECT_EQ(manifest.length, 99u);
  EXPECT_STREQ(path, "untouched");
  EXPECT_FALSE(inventory_hal_test::state.files.contains(DICTIONARY_CACHE_CANDIDATE));
  allowed = true;
  ASSERT_EQ(resolver.resolveBundle("/dictionaries/es", manifest, path), InventoryFileDecision::Include);
  EXPECT_EQ(resolver.resolveBundle("/dictionaries-backup/es", manifest, path), InventoryFileDecision::Skip);
  EXPECT_EQ(resolver.resolveBundle("/dictionaries/.hidden", manifest, path), InventoryFileDecision::Error);
  EXPECT_EQ(resolver.resolveBundle("/dictionaries/es/nested", manifest, path), InventoryFileDecision::Error);
  inventory_hal_test::state.directories["/dictionaries/empty"] = {};
  EXPECT_EQ(resolver.resolveBundle("/dictionaries/empty", manifest, path), InventoryFileDecision::Skip);
}
TEST_F(HalInventoryDictionaryResolverTest, HiddenRootExportsTheSameCanonicalDictionary) {
  auto& state = inventory_hal_test::state;
  state.directories["/.dictionaries/es"] = state.directories.at("/dictionaries/es");
  for (const char* suffix : {".dict", ".idx", ".ifo", ".syn"}) {
    const std::string oldPath = std::string("/dictionaries/es/stem") + suffix;
    state.files[std::string("/.dictionaries/es/stem") + suffix] = state.files.at(oldPath);
  }
  HalInventoryDictionaryResolver resolver(delegate, bindings, scratch);
  ASSERT_TRUE(resolver.prepareAfterRecovery());
  ContentManifest manifest;
  const char* path = nullptr;
  ASSERT_EQ(resolver.resolveBundle("/.dictionaries/es", manifest, path), InventoryFileDecision::Include);
  EXPECT_EQ(bindings.base, "/.dictionaries/es/stem");
  EXPECT_EQ(state.files.at(path), fixture("DictionaryBundle-plain.fixture"));
}
TEST_F(HalInventoryDictionaryResolverTest, DurableBindingDrivesActualScannerAndPendingReplacementFailsClosed) {
  std::array<uint8_t, 256> workspace;
  HalDictionaryCacheStorage cache;
  HalDictionaryBindings durable(cache, workspace);
  HalInventoryDictionaryResolver resolver(delegate, durable, workspace);
  ASSERT_TRUE(resolver.prepareAfterRecovery());
  ContentManifest canonical;
  const char* path = nullptr;
  ASSERT_EQ(resolver.resolveBundle("/dictionaries/es", canonical, path), InventoryFileDecision::Include);
  auto original = fixture("DictionaryBundle-plain.fixture");
  original[original.size() - 2] = 3;
  original.insert(original.end(), {'z', 'i', 'p'});
  inventory_hal_test::state.files[DICTIONARY_CACHE_CANDIDATE] = original;
  ContentManifest transferred = canonical;
  transferred.length = original.size();
  EVP_Digest(original.data(), original.size(), transferred.contentHash.data(), nullptr, EVP_sha256(), nullptr);
  DictionaryCachePublication publication(cache, workspace);
  ASSERT_EQ(publication.publish(transferred), DictionaryCacheResult::Ok);
  const std::string originalPath = publication.publishedPath();
  const DictionaryArchiveBinding binding{canonical, transferred};
  ASSERT_TRUE(durable.install("/dictionaries/es/stem", binding));
  ASSERT_TRUE(durable.finalizeInstallation("/dictionaries/es/stem", binding));
  HalInventoryDirectoryWalker walker;
  Paths paths;
  HalInventoryFileSource source(walker, resolver, paths, workspace);
  ASSERT_TRUE(source.begin());
  ContentManifest selected;
  ASSERT_EQ(source.next(selected), InventorySourceResult::Entry);
  EXPECT_EQ(selected, transferred);
  EXPECT_EQ(paths.entries.back().second, originalPath);
  EXPECT_EQ(source.next(selected), InventorySourceResult::End);
  ASSERT_TRUE(source.closeScan());
  const DictionaryArchiveBinding replacement{canonical, canonical};
  ASSERT_TRUE(durable.install("/dictionaries/es/stem", replacement));
  ASSERT_TRUE(source.begin());
  EXPECT_EQ(source.next(selected), InventorySourceResult::Error);
  EXPECT_EQ(selected, transferred);
  EXPECT_EQ(paths.entries.size(), 1u);
  ASSERT_TRUE(source.closeScan());
  ASSERT_TRUE(durable.finalizeInstallation("/dictionaries/es/stem", replacement));
  ASSERT_TRUE(source.begin());
  ASSERT_EQ(source.next(selected), InventorySourceResult::Entry);
  EXPECT_EQ(selected, canonical);
  EXPECT_EQ(source.next(selected), InventorySourceResult::End);
  EXPECT_EQ(paths.entries.size(), 2u);
}
TEST_F(HalInventoryDictionaryResolverTest, FinalizedContentVerificationReadsMembersAndArchivesWithoutJournalsOrWrites) {
  std::array<uint8_t, 256> workspace{};
  HalDictionaryCacheStorage cache;
  HalDictionaryBindings durable(cache, workspace);
  HalInventoryDictionaryResolver resolver(delegate, durable, workspace);
  ASSERT_TRUE(resolver.prepareAfterRecovery());
  ContentManifest canonical;
  const char* path = nullptr;
  ASSERT_EQ(resolver.resolveBundle("/dictionaries/es", canonical, path), InventoryFileDecision::Include);
  const std::string archivePath(path);
  ASSERT_TRUE(durable.install("/dictionaries/es/stem", {canonical, canonical}));
  ASSERT_TRUE(durable.finalizeInstallation("/dictionaries/es/stem", {canonical, canonical}));
  TransferState state;
  state.phase = TransferPhase::Committed;
  state.owner[0] = 1;
  state.transaction[0] = 2;
  state.storageGeneration[0] = 3;
  state.contentHash = canonical.contentHash;
  state.length = state.durableOffset = canonical.length;
  auto verifier = makeUniqueNoThrow<HalDictionaryFinalizedContentVerification>(workspace);
  ASSERT_TRUE(verifier);
  const auto saved = inventory_hal_test::state.files;
  ASSERT_TRUE(verifier->verify("/dictionaries/es/stem", canonical, state));
  EXPECT_EQ(inventory_hal_test::state.files, saved);
  EXPECT_FALSE(inventory_hal_test::state.files.contains(DICTIONARY_CACHE_CANDIDATE));
  for (const char* member : {"stem.dict", "stem.idx", "stem.ifo", "stem.syn"}) {
    const std::string memberPath = std::string("/dictionaries/es/") + member;
    inventory_hal_test::state.files.at(memberPath)[0] ^= 1;
    const auto damaged = inventory_hal_test::state.files;
    EXPECT_FALSE(verifier->verify("/dictionaries/es/stem", canonical, state)) << member;
    EXPECT_EQ(inventory_hal_test::state.files, damaged);
    inventory_hal_test::state.files = saved;
    ASSERT_TRUE(verifier->verify("/dictionaries/es/stem", canonical, state));
  }
  inventory_hal_test::state.files.at(archivePath)[0] ^= 1;
  EXPECT_FALSE(verifier->verify("/dictionaries/es/stem", canonical, state));
  inventory_hal_test::state.files = saved;
  --state.durableOffset;
  EXPECT_FALSE(verifier->verify("/dictionaries/es/stem", canonical, state));
  ++state.durableOffset;
  state.phase = TransferPhase::Installing;
  EXPECT_FALSE(verifier->verify("/dictionaries/es/stem", canonical, state));
  state.phase = TransferPhase::Committed;
  EXPECT_FALSE(verifier->verify("/dictionaries/es/other", canonical, state));
  inventory_hal_test::state.readErrorPath = "/dictionaries/es/stem.idx";
  EXPECT_FALSE(verifier->verify("/dictionaries/es/stem", canonical, state));
  inventory_hal_test::state.readErrorPath.clear();
  EXPECT_EQ(inventory_hal_test::state.files, saved);
  ASSERT_TRUE(verifier->verify("/dictionaries/es/stem", canonical, state));
}
}  // namespace

TEST_F(HalInventoryDictionaryResolverTest, PreferenceProofValidatesLegacyAndBoundMembersWithoutChangingFiles) {
  std::array<uint8_t, 256> workspace{};
  HalDictionaryCacheStorage cache;
  HalDictionaryBindings durable(cache, workspace);
  HalInventoryDictionaryResolver resolver(delegate, durable, workspace);
  ASSERT_TRUE(resolver.prepareAfterRecovery());
  ContentManifest canonical;
  const char* path = nullptr;
  ASSERT_EQ(resolver.resolveBundle("/dictionaries/es", canonical, path), InventoryFileDecision::Include);
  auto proof = makeUniqueNoThrow<HalReaderPreferenceDictionaryProof>(durable, workspace);
  ASSERT_TRUE(proof);
  const std::array<char, 2> name{'e', 's'};
  const auto legacy = inventory_hal_test::state.files;
  ASSERT_TRUE(proof->verify("/dictionaries/es", name, canonical.contentHash));
  EXPECT_EQ(inventory_hal_test::state.files, legacy);
  auto wrong = canonical.contentHash;
  wrong[0] ^= 1;
  EXPECT_FALSE(proof->verify("/dictionaries/es", name, wrong));
  EXPECT_FALSE(proof->verify("/dictionaries/es", std::span(name).first(1), canonical.contentHash));
  ASSERT_TRUE(durable.install("/dictionaries/es/stem", {canonical, canonical}));
  ASSERT_TRUE(durable.finalizeInstallation("/dictionaries/es/stem", {canonical, canonical}));
  const auto installed = inventory_hal_test::state.files;
  ASSERT_TRUE(proof->verify("/dictionaries/es", name, canonical.contentHash));
  EXPECT_EQ(inventory_hal_test::state.files, installed);
  for (const char* member : {"stem.dict", "stem.idx", "stem.ifo", "stem.syn"}) {
    const std::string memberPath = std::string("/dictionaries/es/") + member;
    inventory_hal_test::state.files.at(memberPath)[0] ^= 1;
    const auto damaged = inventory_hal_test::state.files;
    EXPECT_FALSE(proof->verify("/dictionaries/es", name, canonical.contentHash)) << member;
    EXPECT_EQ(inventory_hal_test::state.files, damaged);
    inventory_hal_test::state.files = installed;
    ASSERT_TRUE(proof->verify("/dictionaries/es", name, canonical.contentHash));
  }
}

TEST_F(HalInventoryDictionaryResolverTest, SelectedPreferenceUsesOriginalIdentityAndRefusesPendingOrDamagedBinding) {
  std::array<uint8_t, 256> workspace{};
  HalDictionaryCacheStorage cache;
  HalDictionaryBindings durable(cache, workspace);
  HalInventoryDictionaryResolver resolver(delegate, durable, workspace);
  ASSERT_TRUE(resolver.prepareAfterRecovery());
  ContentManifest canonical;
  const char* path = nullptr;
  ASSERT_EQ(resolver.resolveBundle("/dictionaries/es", canonical, path), InventoryFileDecision::Include);
  ASSERT_TRUE(durable.install("/dictionaries/es/stem", {canonical, canonical}));
  ASSERT_TRUE(durable.finalizeInstallation("/dictionaries/es/stem", {canonical, canonical}));
  auto original = fixture("DictionaryBundle-plain.fixture");
  original[original.size() - 2] = 3;
  original.insert(original.end(), {'z', 'i', 'p'});
  auto& state = inventory_hal_test::state;
  state.files[DICTIONARY_CACHE_CANDIDATE] = original;
  ContentManifest transferred = canonical;
  transferred.length = original.size();
  EVP_Digest(original.data(), original.size(), transferred.contentHash.data(), nullptr, EVP_sha256(), nullptr);
  DictionaryCachePublication publication(cache, workspace);
  ASSERT_EQ(publication.publish(transferred), DictionaryCacheResult::Ok);
  const std::string originalPath = publication.publishedPath();
  const DictionaryArchiveBinding binding{canonical, transferred};
  ASSERT_TRUE(durable.install("/dictionaries/es/stem", binding));
  auto proof = makeUniqueNoThrow<HalReaderPreferenceDictionaryProof>(durable, workspace);
  ASSERT_TRUE(proof);
  std::array<uint32_t, 256> decoded{};
  std::array<uint32_t, 1024> normalized{};
  std::array<uint8_t, 768> wanted{}, found{};
  HalDictionaryDestinationLookup lookup(decoded, normalized, wanted, found);
  const std::array<char, 2> name{'e', 's'};
  const auto pending = state.files;
  EXPECT_FALSE(proof->verifySelected(lookup, name, transferred.contentHash));
  EXPECT_EQ(state.files, pending);
  ASSERT_TRUE(durable.finalizeInstallation("/dictionaries/es/stem", binding));
  const auto installed = state.files;
  ASSERT_TRUE(proof->inspectSelected(lookup, name));
  ASSERT_NE(proof->contentHash(), nullptr);
  EXPECT_EQ(*proof->contentHash(), transferred.contentHash);
  ASSERT_TRUE(proof->verifySelected(lookup, name, transferred.contentHash));
  EXPECT_FALSE(proof->verifySelected(lookup, name, canonical.contentHash));
  EXPECT_EQ(proof->contentHash(), nullptr);
  state.files.at(originalPath)[0] ^= 1;
  const auto damaged = state.files;
  EXPECT_FALSE(proof->verifySelected(lookup, name, transferred.contentHash));
  EXPECT_EQ(proof->contentHash(), nullptr);
  EXPECT_EQ(state.files, damaged);
  state.files = installed;
  state.directoryErrorPath = "/dictionaries";
  EXPECT_FALSE(proof->verifySelected(lookup, name, transferred.contentHash));
  state.directoryErrorPath.clear();
  ASSERT_TRUE(proof->verifySelected(lookup, name, transferred.contentHash));
  EXPECT_EQ(state.files, installed);
}

TEST_F(HalInventoryDictionaryResolverTest,
       CompressedPreferenceProofRequiresBorrowedResourcesAndPreservesFilesOnFailure) {
  auto& state = inventory_hal_test::state;
  state.files.erase("/dictionaries/es/stem.dict");
  state.files["/dictionaries/es/stem.dict.dz"] = fixture("dictzip-chunks.dict.dz");
  state.directories["/dictionaries/es"][2].name = "stem.dict.dz";
  auto window = makeUniqueNoThrow<uint8_t[]>(32768);
  auto decoder = makeUniqueNoThrow<tinfl_decompressor>();
  ASSERT_TRUE(window);
  ASSERT_TRUE(decoder);
  std::array<uint8_t, 256> workspace{};
  HalDictionaryCacheStorage cache;
  HalDictionaryBindings durable(cache, workspace);
  HalInventoryDictionaryResolver resolver(delegate, durable, workspace, decoder.get(), std::span(window.get(), 32768));
  ASSERT_TRUE(resolver.prepareAfterRecovery());
  ContentManifest canonical;
  const char* path = nullptr;
  ASSERT_EQ(resolver.resolveBundle("/dictionaries/es", canonical, path), InventoryFileDecision::Include);
  ASSERT_TRUE(durable.install("/dictionaries/es/stem", {canonical, canonical}));
  ASSERT_TRUE(durable.finalizeInstallation("/dictionaries/es/stem", {canonical, canonical}));
  const auto saved = state.files;
  const std::array<char, 2> name{'e', 's'};
  auto missing = makeUniqueNoThrow<HalReaderPreferenceDictionaryProof>(durable, workspace);
  ASSERT_TRUE(missing);
  EXPECT_FALSE(missing->verify("/dictionaries/es", name, canonical.contentHash));
  EXPECT_EQ(state.files, saved);
  missing.reset();
  struct Provider {
    tinfl_decompressor* decoder;
    std::span<uint8_t> window;
    unsigned calls = 0;
    bool allowed = false;
  } provider{decoder.get(), std::span(window.get(), 32768)};
  auto lazy = makeUniqueNoThrow<HalReaderPreferenceDictionaryProof>(
      durable, workspace, nullptr, std::span<uint8_t>{}, nullptr, nullptr,
      [](void* context, tinfl_decompressor*& output, std::span<uint8_t>& bytes) {
        auto& provider = *static_cast<Provider*>(context);
        ++provider.calls;
        output = provider.decoder;
        bytes = provider.window;
        return provider.allowed;
      },
      &provider);
  ASSERT_TRUE(lazy);
  EXPECT_FALSE(lazy->verify("/dictionaries/es", name, canonical.contentHash));
  EXPECT_EQ(lazy->contentHash(), nullptr);
  EXPECT_EQ(provider.calls, 1U);
  EXPECT_EQ(state.files, saved);
  provider.allowed = true;
  ASSERT_TRUE(lazy->verify("/dictionaries/es", name, canonical.contentHash));
  EXPECT_EQ(provider.calls, 2U);
  ASSERT_TRUE(lazy->verify("/dictionaries/es", name, canonical.contentHash));
  EXPECT_EQ(provider.calls, 2U);
  EXPECT_EQ(state.files, saved);
  lazy.reset();
  auto proof = makeUniqueNoThrow<HalReaderPreferenceDictionaryProof>(durable, workspace, decoder.get(),
                                                                     std::span(window.get(), 32768));
  ASSERT_TRUE(proof);
  ASSERT_TRUE(proof->verify("/dictionaries/es", name, canonical.contentHash));
  EXPECT_EQ(state.files, saved);
  state.failRead = state.reads + 1;
  EXPECT_FALSE(proof->verify("/dictionaries/es", name, canonical.contentHash));
  EXPECT_EQ(state.files, saved);
  state.failRead = 0;
  state.files["/dictionaries/es/stem.dict.dz"][0] ^= 1;
  const auto damaged = state.files;
  EXPECT_FALSE(proof->verify("/dictionaries/es", name, canonical.contentHash));
  EXPECT_EQ(state.files, damaged);
  state.files = saved;
  ASSERT_TRUE(proof->verify("/dictionaries/es", name, canonical.contentHash));
  EXPECT_EQ(state.files, saved);
}
