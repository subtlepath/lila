#pragma once

#include "CompanionDictionaryArchiveBinding.h"
#include "CompanionDictionaryMembersValidation.h"
#include "HalDictionaryArchiveStage.h"
#include "HalDictionaryBundleSource.h"
#include "HalDictionaryCacheStorage.h"
#include "HalDictionaryDiscovery.h"
#include "HalInventoryFileSource.h"

namespace companion {
// Session-owned outside the task stack. Scratch, decoder and 32-KiB window are
// borrowed; the serialized owner prevents member changes during validation/build.
class HalInventoryDictionaryResolver final : public InventoryFileResolver {
 public:
  HalInventoryDictionaryResolver(InventoryFileResolver& delegate, DictionaryArchiveBindings& bindings,
                                 std::span<uint8_t> scratch, tinfl_decompressor* decoder = nullptr,
                                 std::span<uint8_t> window = {}, InventoryHashProgress progress = nullptr,
                                 void* context = nullptr)
      : delegate(delegate),
        bindings(bindings),
        discovery(progress, context),
        validation(source, scratch, decoder, window, progress, context),
        stage(scratch, progress, context),
        builder(stage, scratch),
        storage(progress, context),
        publication(storage, scratch) {}
  // Call after authoritative inventory recovery, with exclusive cache ownership.
  bool prepareAfterRecovery() {
    ready = false;
    prepared = publication.discardUnpublishedCandidate() == DictionaryCacheResult::Ok;
    return prepared;
  }
  InventoryFileDecision resolve(const char* path, HalFile& file, ContentManifest& metadata) override {
    ready = false;
    const auto relative = dictionaryRelative(path);
    if (!relative) return delegate.resolve(path, file, metadata);
    if (!*relative) return file.isDirectory() ? InventoryFileDecision::Include : failure("Dictionary root is a file");
    if (*relative == '.' || std::strchr(relative, '/')) return InventoryFileDecision::Skip;
    return file.isDirectory() ? InventoryFileDecision::Include : InventoryFileDecision::Skip;
  }
  InventoryFileDecision resolveBundle(const char* path, ContentManifest& metadata, const char*& archivePath) override {
    ready = false;
    const auto relative = dictionaryRelative(path);
    if (!relative || !*relative) return delegate.resolveBundle(path, metadata, archivePath);
    if (*relative == '.' || std::strchr(relative, '/')) return failure("Invalid dictionary bundle folder");
    if (!prepared) return failure("Dictionary cache has not been recovered");
    DictionaryDiscoveryDetails discovered;
    const auto result = discovery.inspect(path, discovered);
    if (result == DictionaryDiscoveryResult::Empty) return InventoryFileDecision::Skip;
    if (result != DictionaryDiscoveryResult::Found) return failure("Dictionary folder discovery failed");
    DictionaryMembersDetails details;
    if (!source.begin(discovery.basePath(), discovered.compressed, discovered.synonyms) ||
        !validation.validate(discovered.compressed, discovered.synonyms, details) || !source.reopen())
      return failure("Dictionary members validation failed");
    canonical = {};
    canonical.kind = ContentKind::Dictionary;
    canonical.formatVersion = 1;
    if (!builder.build(source, discovered.compressed, discovered.synonyms, canonical.length) || !stage.isSealed())
      return failure("Dictionary archive construction failed");
    canonical.contentHash = stage.contentHash();
    if (publication.publish(canonical) != DictionaryCacheResult::Ok)
      return failure("Dictionary archive publication failed");
    const auto bound = bindings.read(discovery.basePath(), binding);
    if (bound == DictionaryBindingResult::Error) return failure("Dictionary archive binding is unreadable");
    selected = canonical;
    if (bound == DictionaryBindingResult::Found) {
      if (binding.members != canonical) return failure("Dictionary archive binding does not match installed members");
      if (publication.find(binding.original) != DictionaryCacheResult::Ok)
        return failure("Retained original dictionary archive is unavailable");
      selected = binding.original;
    } else if (bound != DictionaryBindingResult::Missing) {
      return failure("Invalid dictionary binding result");
    }
    ready = true;
    metadata = selected;
    archivePath = publication.publishedPath();
    return InventoryFileDecision::Include;
  }
  bool verifyHashed(const char* path, const ContentManifest& manifest) override {
    if (!ready) return delegate.verifyHashed(path, manifest);
    return path && publication.publishedPath() && !std::strcmp(path, publication.publishedPath()) &&
           manifest == selected;
  }

 private:
  InventoryFileResolver& delegate;
  DictionaryArchiveBindings& bindings;
  HalDictionaryDiscovery discovery;
  HalDictionaryBundleSource source;
  DictionaryMembersValidation validation;
  HalDictionaryArchiveStage stage;
  DictionaryBundleBuilder builder;
  HalDictionaryCacheStorage storage;
  DictionaryCachePublication publication;
  DictionaryArchiveBinding binding;
  ContentManifest canonical, selected;
  bool prepared = false, ready = false;
  static const char* dictionaryRelative(const char* path) {
    if (!path) return nullptr;
    static constexpr const char* ROOTS[] = {"/dictionaries", "/.dictionaries"};
    for (const auto root : ROOTS) {
      const auto length = std::strlen(root);
      if (!std::strncmp(path, root, length) && (path[length] == 0 || path[length] == '/'))
        return path + length + (path[length] == '/');
    }
    return nullptr;
  }
  InventoryFileDecision failure(const char* reason) {
    ready = false;
    LOG_ERR("COMPANION", "%s", reason);
    return InventoryFileDecision::Error;
  }
};
}  // namespace companion
