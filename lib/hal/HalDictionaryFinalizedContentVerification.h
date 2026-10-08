#pragma once

#include "CompanionDictionaryInstallationPlan.h"
#include "CompanionDictionaryInstallationTemporaries.h"
#include "HalDictionaryBindings.h"
#include "HalDictionaryBundleHashStage.h"
#include "HalDictionaryBundleSource.h"
#include "HalDictionaryCacheStorage.h"
#include "HalDictionaryDiscovery.h"

namespace companion {
// Retained outside the task stack. Borrow wire scratch and exclusive installed
// member/cache ownership. Verification performs reads only, without journals.
class HalDictionaryFinalizedContentVerification final {
 public:
  explicit HalDictionaryFinalizedContentVerification(std::span<uint8_t> scratch,
                                                     InventoryHashProgress progress = nullptr, void* context = nullptr)
      : digest(progress, context),
        discovery(progress, context),
        cache(progress, context),
        bindings(cache, scratch, progress, context),
        builder(digest, scratch) {}
  bool verifyRetired(const char* destination, const ContentManifest& manifest, const TransferState& state) {
    for (const auto path : DICTIONARY_INSTALLATION_TEMPORARIES)
      if (lookup.inspect(path) != CompanionFilePresence::Missing) return fail("remaining installation temporary");
    for (const auto path : DICTIONARY_RETIREMENT_JOURNALS)
      if (lookup.inspect(path) != CompanionFilePresence::Missing) return fail("remaining retirement proof");
    return verify(destination, manifest, state);
  }
  bool verify(const char* destination, const ContentManifest& manifest, const TransferState& state) {
    if (!destination || !validDictionaryInstallationBase(std::string_view(destination, strnlen(destination, 128))) ||
        !validDictionaryBindingManifest(manifest) || !matchesTransferManifest(manifest, state) ||
        state.phase != TransferPhase::Committed || state.durableOffset != state.length ||
        !inventory_detail::nonzero(state.owner) || !inventory_detail::nonzero(state.transaction) ||
        !inventory_detail::nonzero(state.storageGeneration))
      return fail("transfer context");
    if (bindings.read(destination, binding) != DictionaryBindingResult::Found || binding.original != manifest ||
        !bindings.verifyFinalized(destination, binding))
      return fail("binding or retained archives");
    const std::string_view base(destination);
    if (discovery.inspect(base.substr(0, base.find_last_of('/')), details) != DictionaryDiscoveryResult::Found ||
        std::strcmp(discovery.basePath(), destination))
      return fail("destination members");
    if (!source.begin(base, details.compressed, details.synonyms) ||
        !builder.build(source, details.compressed, details.synonyms, length) || length != binding.members.length ||
        !digest.contentHash() || *digest.contentHash() != binding.members.contentHash)
      return fail("installed canonical archive");
    return true;
  }

 private:
  HalDictionaryBundleHashStage digest;
  class ReadOnlyCache final : public DictionaryCacheStorage {
   public:
    ReadOnlyCache(InventoryHashProgress progress, void* context) : storage(progress, context) {}
    bool prepare() override { return Storage.ready() || mutation("SD unavailable"); }
    DictionaryCacheCheck inspect(const char* path, const ContentManifest& manifest,
                                 std::span<uint8_t> scratch) override {
      return storage.inspect(path, manifest, scratch);
    }
    bool rename(const char*, const char*) override { return mutation("rename"); }
    bool remove(const char*) override { return mutation("remove"); }

   private:
    HalDictionaryCacheStorage storage;
    static bool mutation(const char* operation) {
      LOG_ERR("COMPANION", "Read-only dictionary cache rejected %s", operation);
      return false;
    }
  };
  HalDictionaryDiscovery discovery;
  HalCompanionFileLookup lookup;
  ReadOnlyCache cache;
  HalDictionaryBindings bindings;
  HalDictionaryBundleSource source;
  DictionaryBundleBuilder builder;
  DictionaryArchiveBinding binding;
  DictionaryDiscoveryDetails details;
  uint64_t length = 0;
  static bool fail(const char* reason) {
    LOG_ERR("COMPANION", "Finalized dictionary %s failed", reason);
    return false;
  }
};
}  // namespace companion
