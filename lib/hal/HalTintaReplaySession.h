#pragma once

#include <Memory.h>

#include "HalJournalCausalAuditSession.h"
#include "HalTintaReplayStore.h"

namespace companion {
// Caller owns this outside the task stack and freezes journal, catalog and writers.
class HalTintaReplaySession {
 public:
  bool run(const Identity& course, TintaSubjectCatalog& catalog) {
    ready = false;
    frontier.fill(0);
    if (!store.close() || !tinta_body_detail::nonzero(course)) return failure("arguments or close");
    // Audit scratch and reducer state exceed the task-local budget. Each is allocated once.
    auto audit = makeUniqueNoThrow<HalJournalCausalAuditSession>();
    if (!audit) return failure("OOM: audit workspace");
    if (!audit->run(&computed, &course, &catalog)) return failure("journal audit");
    if (!store.begin(course)) return failure("working store");
    auto reducer = makeUniqueNoThrow<TintaReplayReducer>(store, course);
    if (!reducer) {
      store.close();
      return failure("OOM: reducer workspace");
    }
    if (!audit->replay(reducer.get(), reduce, true)) {
      store.close();
      return failure("journal replay");
    }
    frontier = computed;
    ready = true;
    return true;
  }
  // Borrowed results remain available only until the next run or owner destruction.
  HalTintaReplayStore* workingStore() { return ready ? &store : nullptr; }
  const Digest* journalFrontier() const { return ready ? &frontier : nullptr; }

 private:
  static bool reduce(void* context, uint32_t, const SyncEvent& event, std::span<const uint8_t> bytes, bool undone) {
    const auto result = static_cast<TintaReplayReducer*>(context)->apply(event, bytes, undone);
    if (result == TintaJournalResult::Ok) return true;
    LOG_ERR("COMPANION", "Tinta replay reduction failed: %u", static_cast<unsigned>(result));
    return false;
  }
  static bool failure(const char* stage) {
    LOG_ERR("COMPANION", "Tinta replay session %s failed", stage);
    return false;
  }
  HalTintaReplayStore store;
  Digest frontier{}, computed{};
  bool ready = false;
};
}  // namespace companion
