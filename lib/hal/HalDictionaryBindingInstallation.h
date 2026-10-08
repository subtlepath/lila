#pragma once

#include "CompanionDictionaryExtractionParent.h"
#include "HalDictionaryBindings.h"
#include "HalDictionaryStagedArchive.h"

namespace companion {
// The serialized installer supplies the base path from its durable installation
// plan and retains the canonical build proof until commit/finalization.
class HalDictionaryBindingInstallation final {
 public:
  explicit HalDictionaryBindingInstallation(HalDictionaryBindings& bindings) : bindings(bindings) {}
  bool install(const char* basePath, const ContentManifest& original, const HalDictionaryStagedArchive& canonical,
               const DictionaryExtractionParent& parent) {
    if (!prepare(original, canonical, parent, TransferPhase::Installing)) return false;
    return bindings.install(basePath, binding);
  }
  bool finalize(const char* basePath, const ContentManifest& original, const HalDictionaryStagedArchive& canonical,
                const DictionaryExtractionParent& parent) {
    if (!prepare(original, canonical, parent, TransferPhase::Committed)) return false;
    return bindings.finalizeInstallation(basePath, binding);
  }

 private:
  HalDictionaryBindings& bindings;
  DictionaryArchiveBinding binding;
  bool prepare(const ContentManifest& original, const HalDictionaryStagedArchive& canonical,
               const DictionaryExtractionParent& parent, TransferPhase phase) {
    const auto receipt = parent.current();
    const auto built = canonical.current();
    if (!receipt || !built || !parent.isPhase(phase) || !parent.matchesArchive(original) ||
        !canonical.matchesReceipt(*receipt) || !validDictionaryBindingManifest(*built)) {
      LOG_ERR("COMPANION", "Dictionary binding parent or canonical proof mismatch");
      return false;
    }
    binding = {*built, original};
    return true;
  }
};
}  // namespace companion
