#pragma once

#include "CompanionDictionaryInstallationPlan.h"
#include "HalDictionaryStagedArchive.h"

namespace companion {
// Session-owned working plan preserves caller output and permits borrowed input
// views into that output. Authorize against the recovered transfer before save.
class HalDictionaryInstallationPlanBuilder final {
 public:
  bool create(std::string_view base, const ContentManifest& original, const HalDictionaryStagedArchive& canonical,
              const DictionaryExtractionParent& parent, DictionaryInstallationPlan& output) {
    const auto receipt = parent.current();
    const auto built = canonical.current();
    if (!receipt || !built || !validDictionaryInstallationBase(base) || !parent.matchesArchive(original) ||
        !validDictionaryBindingManifest(original) || !validDictionaryBindingManifest(*built) ||
        !canonical.matchesReceipt(*receipt)) {
      LOG_ERR("COMPANION", "Dictionary installation plan proof or destination mismatch");
      return false;
    }
    working.revision = 1;
    working.phase = DictionaryInstallationPhase::Prepared;
    working.published = 0;
    working.extraction = *receipt;
    working.archives = {*built, original};
    working.base.fill(0);
    std::copy(base.begin(), base.end(), working.base.begin());
    if (!validDictionaryInstallationPlan(working)) {
      LOG_ERR("COMPANION", "Invalid dictionary installation plan");
      return false;
    }
    output = working;
    return true;
  }

 private:
  DictionaryInstallationPlan working;
};
}  // namespace companion
