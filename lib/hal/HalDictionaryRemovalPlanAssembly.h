#pragma once

#include "CompanionDictionaryRemovalPlan.h"
#include "HalDictionaryBindings.h"
#include "HalDictionaryBundleHashStage.h"
#include "HalDictionaryBundleSource.h"
#include "HalDictionaryDiscovery.h"
#include "HalDictionaryRemovalMemberProofSource.h"

namespace companion {
enum class DictionaryRemovalAssemblyResult { Match, Unrelated, Empty, Error };
// Retain outside the task stack. Inventory has semantically validated the selected
// archive; the caller guards card/revision and excludes member/cache/binding writers.
// Scratch and cache are borrowed. Proofs hash the same bytes as the canonical ZIP.
class HalDictionaryRemovalPlanAssembly final {
 public:
  HalDictionaryRemovalPlanAssembly(DictionaryCacheStorage& cache, std::span<uint8_t> scratch,
                                   InventoryHashProgress progress = nullptr, void* context = nullptr)
      : cache(cache),
        bindings(this->cache, scratch, progress, context),
        publication(this->cache, scratch),
        discovery(progress, context),
        proofs(source),
        digest(progress, context),
        builder(digest, scratch),
        scratch(scratch),
        progress(progress),
        context(context) {}
  bool closeReaders() {
    const bool bindingClosed = bindings.closeReaders();
    const bool sourceClosed = source.close();
    const bool discoveryClosed = discovery.closeReaders();
    return bindingClosed && sourceClosed && discoveryClosed;
  }
  DictionaryRemovalAssemblyResult assemble(std::string_view folder, const ContentRemovalRequest& request,
                                           DictionaryRemovalPlan& output) {
    const auto a = reinterpret_cast<uintptr_t>(&output), b = reinterpret_cast<uintptr_t>(scratch.data());
    if (scratch.size() < DICTIONARY_BINDING_SIZE || !(a <= b ? sizeof(output) <= b - a : scratch.size() <= a - b))
      return error("scratch/output bounds");
    if (!validContentRemovalRequest(request) || request.manifest.kind != ContentKind::Dictionary || !guard())
      return error("request");
    const auto found = discovery.inspect(folder, details);
    if (found == DictionaryDiscoveryResult::Empty)
      return guard() ? DictionaryRemovalAssemblyResult::Empty : error("card");
    if (found != DictionaryDiscoveryResult::Found || !guard() || !validDictionaryInstallationBase(discovery.basePath()))
      return error("discovery/base");
    proofs.reset(details.synonyms);
    canonical = {};
    canonical.kind = ContentKind::Dictionary;
    canonical.formatVersion = 1;
    if (!source.begin(discovery.basePath(), details.compressed, details.synonyms) ||
        !builder.build(proofs, details.compressed, details.synonyms, canonical.length) || !digest.contentHash() ||
        !proofs.finish() || !guard()) {
      source.close();
      return error("canonical/member proofs");
    }
    canonical.contentHash = *digest.contentHash();
    if (publication.find(canonical) != DictionaryCacheResult::Ok || !guard()) return error("canonical cache");
    const auto bound = bindings.read(discovery.basePath(), binding);
    if (!guard() || bound == DictionaryBindingResult::Error) return error("binding read");
    selected = canonical;
    if (bound == DictionaryBindingResult::Found) {
      if (binding.members != canonical || !bindings.verifyFinalized(discovery.basePath(), binding) || !guard())
        return error("binding/cache proof");
      selected = binding.original;
    }
    if (selected != request.manifest) return DictionaryRemovalAssemblyResult::Unrelated;
    candidate = {};
    candidate.request = request;
    auto& installed = candidate.installed;
    installed.revision = 1;
    installed.phase = DictionaryInstallationPhase::Committed;
    installed.archives = {canonical, selected};
    std::strcpy(installed.base.data(), discovery.basePath());
    auto& receipt = installed.extraction;
    receipt.revision = 1;
    receipt.transaction = request.transaction;
    receipt.generation = request.generation;
    receipt.archiveHash = selected.contentHash;
    receipt.compressed = details.compressed;
    receipt.synonyms = details.synonyms;
    receipt.sealed = installed.published = details.synonyms ? 15 : 7;
    receipt.lengths = proofs.memberLengths();
    receipt.hashes = proofs.memberHashes();
    if (!validDictionaryRemovalPlan(candidate) || !guard()) return error("assembled proof");
    output = candidate;
    return DictionaryRemovalAssemblyResult::Match;
  }

 private:
  class ReadOnlyCache final : public DictionaryCacheStorage {
   public:
    explicit ReadOnlyCache(DictionaryCacheStorage& storage) : storage(storage) {}
    bool prepare() override { return Storage.ready(); }
    DictionaryCacheCheck inspect(const char* path, const ContentManifest& manifest,
                                 std::span<uint8_t> scratch) override {
      return storage.inspect(path, manifest, scratch);
    }
    bool rename(const char*, const char*) override { return mutation(); }
    bool remove(const char*) override { return mutation(); }

   private:
    DictionaryCacheStorage& storage;
    static bool mutation() {
      LOG_ERR("COMPANION", "Read-only dictionary removal cache mutation refused");
      return false;
    }
  } cache;
  HalDictionaryBindings bindings;
  DictionaryCachePublication publication;
  HalDictionaryDiscovery discovery;
  HalDictionaryBundleSource source;
  HalDictionaryRemovalMemberProofSource proofs;
  HalDictionaryBundleHashStage digest;
  DictionaryBundleBuilder builder;
  DictionaryArchiveBinding binding;
  ContentManifest canonical, selected;
  DictionaryDiscoveryDetails details;
  DictionaryRemovalPlan candidate;
  std::span<uint8_t> scratch;
  InventoryHashProgress progress;
  void* context;
  bool guard() const { return !progress || progress(context); }
  static DictionaryRemovalAssemblyResult error(const char* reason) {
    LOG_ERR("COMPANION", "Dictionary removal assembly %s failed", reason);
    return DictionaryRemovalAssemblyResult::Error;
  }
};
}  // namespace companion
