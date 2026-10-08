#pragma once

#include "CompanionJournalCourseMembership.h"
#include "CompanionTintaMarkJournal.h"
#include "CompanionTintaNativeMarkIdentity.h"
#include "HalTintaMutationClock.h"

namespace companion {
// Borrow a validated immutable pack/catalog and retain until native mark logs close.
class HalTintaMarkMutationContext {
 public:
  HalTintaMarkMutationContext(const tinta::core::pack::Pack& pack, TintaSubjectCatalog& catalog,
                              const tinta::platform::Clock& clock, EventKind kind, void* recoveryContext,
                              bool (*recover)(void*), void (*reportError)(void*, TintaJournalResult))
      : pack(pack),
        catalog(catalog),
        clock(clock),
        kind(kind),
        recoveryContext(recoveryContext),
        recover(recover),
        reportError(reportError) {}
  TintaMarkJournal::Callbacks callbacks() {
    if (!recover || !reportError) return {};
    return {this, &resolveCallback, &clockCallback, &recoverCallback, &errorCallback};
  }
  bool resolve(uint32_t key, uint32_t& output) {
    return resolveTintaNativeMarkIdentity(pack, catalog, kind, key, output);
  }

 private:
  static bool resolveCallback(void* context, uint32_t key, uint32_t& identity) {
    return static_cast<HalTintaMarkMutationContext*>(context)->resolve(key, identity);
  }
  static bool clockCallback(void* context, uint32_t& day, uint64_t& timestamp, ClockQuality& quality) {
    return readTintaMutationClock(static_cast<HalTintaMarkMutationContext*>(context)->clock, day, timestamp, quality);
  }
  static bool recoverCallback(void* context) {
    auto& owner = *static_cast<HalTintaMarkMutationContext*>(context);
    return owner.recover && owner.recover(owner.recoveryContext);
  }
  static void errorCallback(void* context, TintaJournalResult result) {
    auto& owner = *static_cast<HalTintaMarkMutationContext*>(context);
    if (owner.reportError) owner.reportError(owner.recoveryContext, result);
  }
  const tinta::core::pack::Pack& pack;
  TintaSubjectCatalog& catalog;
  const tinta::platform::Clock& clock;
  EventKind kind;
  void* recoveryContext;
  bool (*recover)(void*);
  void (*reportError)(void*, TintaJournalResult);
};
}  // namespace companion
