#pragma once

#include "CompanionDictionaryMembersValidation.h"
#include "HalDictionaryBindings.h"
#include "HalDictionaryBundleHashStage.h"
#include "HalDictionaryBundleSource.h"
#include "HalDictionaryDestinationLookup.h"
#include "HalDictionaryDiscovery.h"

namespace companion {
// Retain off-stack. The caller excludes member/binding/cache mutations and
// borrows the shared scratch plus decoder/window only for compressed bundles.
class HalReaderPreferenceDictionaryProof final {
 public:
  using DecoderProvider = bool (*)(void*, tinfl_decompressor*&, std::span<uint8_t>&);
  HalReaderPreferenceDictionaryProof(HalDictionaryBindings& bindings, std::span<uint8_t> scratch,
                                     tinfl_decompressor* decoder = nullptr, std::span<uint8_t> window = {},
                                     InventoryHashProgress progress = nullptr, void* context = nullptr,
                                     DecoderProvider decoderProvider = nullptr, void* decoderContext = nullptr)
      : bindings(bindings),
        discovery(progress, context),
        validation(source, scratch, decoder, window, progress, context),
        digest(progress, context),
        builder(digest, scratch),
        decoder(decoder),
        window(window),
        decoderProvider(decoderProvider),
        decoderContext(decoderContext) {}
  bool verifySelected(HalDictionaryDestinationLookup& lookup, std::span<const char> name,
                      std::span<const uint8_t> expectedHash) {
    if (expectedHash.size() != 32) return failure("selection hash");
    if (!inspectSelected(lookup, name)) return false;
    return std::equal(expectedHash.begin(), expectedHash.end(), selectedHash.begin()) || failure("selected hash");
  }
  bool inspectSelected(HalDictionaryDestinationLookup& lookup, std::span<const char> name) {
    ready = false;
    digest.abort();
    if (name.empty() || name.size() > 31) return failure("selection arguments");
    if (lookup.resolveReaderFolder(std::string_view(name.data(), name.size()), selectedFolder) !=
        DictionaryDestinationPresence::Present)
      return failure("folder selection");
    return inspectFolder(std::string_view(selectedFolder.data()), name);
  }
  bool verify(std::string_view folder, std::span<const char> name, std::span<const uint8_t> expectedHash) {
    if (expectedHash.size() != 32) return failure("expected hash");
    if (!inspectFolder(folder, name)) return false;
    return std::equal(expectedHash.begin(), expectedHash.end(), selectedHash.begin()) || failure("selected hash");
  }
  const Digest* contentHash() const { return ready ? &selectedHash : nullptr; }

 private:
  bool inspectFolder(std::string_view folder, std::span<const char> name) {
    ready = false;
    digest.abort();
    if (!validInventoryPath(folder) || name.empty() || name.size() > 31) return failure("arguments");
    const auto root = folder.starts_with("/dictionaries/")    ? size_t{14}
                      : folder.starts_with("/.dictionaries/") ? size_t{15}
                                                              : 0;
    if (!root || folder.substr(root) != std::string_view(name.data(), name.size())) return failure("selected folder");
    if (discovery.inspect(folder, discovered) != DictionaryDiscoveryResult::Found) return failure("folder discovery");
    if (discovered.compressed && (!decoder || window.size() != 32768)) {
      auto* suppliedDecoder = decoder;
      auto suppliedWindow = window;
      if (!decoderProvider || !decoderProvider(decoderContext, suppliedDecoder, suppliedWindow) || !suppliedDecoder ||
          suppliedWindow.size() != 32768)
        return failure("compressed resources");
      decoder = suppliedDecoder;
      window = suppliedWindow;
      validation.setCompressedResources(decoder, window);
    }
    if (!source.begin(discovery.basePath(), discovered.compressed, discovered.synonyms) ||
        !validation.validate(discovered.compressed, discovered.synonyms, details) || !source.reopen())
      return failure("installed members");
    canonical = {};
    canonical.kind = ContentKind::Dictionary;
    canonical.formatVersion = 1;
    if (!builder.build(source, discovered.compressed, discovered.synonyms, canonical.length) || !digest.contentHash())
      return failure("canonical hash");
    canonical.contentHash = *digest.contentHash();
    const auto result = bindings.read(discovery.basePath(), binding);
    if (result == DictionaryBindingResult::Missing) {
      selectedHash = canonical.contentHash;
      ready = true;
      return true;
    }
    if (result != DictionaryBindingResult::Found || binding.members != canonical ||
        !bindings.verifyFinalized(discovery.basePath(), binding))
      return failure("binding/original archives");
    selectedHash = binding.original.contentHash;
    ready = true;
    return true;
  }

 private:
  bool failure(const char* reason) {
    ready = false;
    LOG_ERR("COMPANION", "Reader dictionary preference proof failed: %s", reason);
    return false;
  }
  HalDictionaryBindings& bindings;
  HalDictionaryDiscovery discovery;
  HalDictionaryBundleSource source;
  DictionaryMembersValidation validation;
  HalDictionaryBundleHashStage digest;
  DictionaryBundleBuilder builder;
  DictionaryDiscoveryDetails discovered;
  DictionaryMembersDetails details;
  ContentManifest canonical;
  DictionaryArchiveBinding binding;
  std::array<char, 64> selectedFolder{};
  Digest selectedHash{};
  bool ready = false;
  tinfl_decompressor* decoder;
  std::span<uint8_t> window;
  DecoderProvider decoderProvider;
  void* decoderContext;
};
}  // namespace companion
