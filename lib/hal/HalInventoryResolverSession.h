#pragma once

#include "HalDictionaryBindings.h"
#include "HalInventoryBaseResolver.h"
#include "HalInventoryBuildSession.h"
#include "HalInventoryCourseResolver.h"
#include "HalInventoryDictionaryResolver.h"
#include "HalInventoryFontResolver.h"

namespace companion {
// Session-owned outside the stack. Resolving and hashing run sequentially;
// the hash bank is shared, while the DEFLATE window remains separate.
class HalInventoryResolverSession final : public InventoryFileResolver {
 public:
  HalInventoryResolverSession(TransferStorage& storage, std::span<uint8_t> workspace, tinfl_decompressor& decoder,
                              std::span<uint8_t> window)
      : workspace(workspace),
        window(window),
        fonts(base, hashBank(workspace)),
        bindings(cache, hashBank(workspace)),
        dictionaries(fonts, bindings, hashBank(workspace), &decoder, window),
#if LILA_TINTA
        courses(storage, pack, dictionaries, hashBank(workspace))
#else
        courses(dictionaries)
#endif
  {
#if !LILA_TINTA
    (void)storage;
#endif
  }
  // The caller must recover authoritative inventory/cache state first.
  bool prepareAfterRecovery() {
    prepared = false;
    if (workspace.size() < HalInventoryBuildSession::WORKSPACE_SIZE || window.size() != 32768)
      return failure("workspace");
    prepared = dictionaries.prepareAfterRecovery();
    return prepared || failure("dictionary preparation");
  }
  InventoryFileDecision resolve(const char* path, HalFile& file, ContentManifest& metadata) override {
    if (!prepared) {
      failure("not prepared");
      return InventoryFileDecision::Error;
    }
    return courses.resolve(path, file, metadata);
  }
  InventoryFileDecision resolveBundle(const char* path, ContentManifest& metadata, const char*& archivePath) override {
    if (!prepared) {
      failure("bundle not prepared");
      return InventoryFileDecision::Error;
    }
    return courses.resolveBundle(path, metadata, archivePath);
  }
  bool verifyHashed(const char* path, const ContentManifest& manifest) override {
    return (prepared && courses.verifyHashed(path, manifest)) || failure("hashed binding");
  }

 private:
  static std::span<uint8_t> hashBank(std::span<uint8_t> bytes) {
    return bytes.first(std::min(bytes.size(), HalInventoryBuildSession::HASH_SIZE));
  }
  std::span<uint8_t> workspace, window;
  HalInventoryBaseResolver base;
  HalInventoryFontResolver fonts;
  HalDictionaryCacheStorage cache;
  HalDictionaryBindings bindings;
  HalInventoryDictionaryResolver dictionaries;
#if LILA_TINTA
  tinta::core::pack::Pack pack;
#endif
  HalInventoryCourseResolver courses;
  bool prepared = false;
  static bool failure(const char* reason) {
    LOG_ERR("COMPANION", "Inventory resolver session failed: %s", reason);
    return false;
  }
};
}  // namespace companion
