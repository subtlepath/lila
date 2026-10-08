#pragma once

#include "CompanionDictionaryPlanBinding.h"
#include "HalDictionaryBindings.h"

namespace companion {
// The plan coordinator supplies parent authorization and installed-member proof.
class HalDictionaryPlanBindingStorage final : public DictionaryPlanBindingStorage {
 public:
  explicit HalDictionaryPlanBindingStorage(HalDictionaryBindings& bindings) : bindings(bindings) {}
  bool install(const DictionaryInstallationPlan& plan) override {
    if (!validDictionaryInstallationPlan(plan)) {
      LOG_ERR("COMPANION", "Invalid dictionary binding installation plan");
      return false;
    }
    return bindings.install(plan.base.data(), plan.archives);
  }
  bool finalize(const DictionaryInstallationPlan& plan) override {
    if (!validDictionaryInstallationPlan(plan)) {
      LOG_ERR("COMPANION", "Invalid dictionary binding finalization plan");
      return false;
    }
    return bindings.finalizeInstallation(plan.base.data(), plan.archives);
  }

 private:
  HalDictionaryBindings& bindings;
};
}  // namespace companion
