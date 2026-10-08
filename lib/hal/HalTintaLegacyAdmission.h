#pragma once

#include <Memory.h>

#include "HalJournalCausalAuditSession.h"

namespace companion {
inline bool allowLegacyTintaWithoutReceipt(const Identity& course) {
  const auto failure = [](const char* reason) {
    LOG_ERR("COMPANION", "Tinta legacy admission failed: %s", reason);
    return false;
  };
  if (!tinta_body_detail::nonzero(course)) return failure("course identity");
  {
    // Lookup handles/name buffers exceed the task stack; release before audit.
    auto lookup = makeUniqueNoThrow<HalCompanionFileLookup>();
    if (!lookup) return failure("OOM: journal lookup");
    const auto presence = lookup->inspect(TINTA_JOURNAL_DIRECTORY);
    if (presence == CompanionFilePresence::Missing) return true;
    if (presence != CompanionFilePresence::Present) return failure("journal presence");
  }
  auto audit = makeUniqueNoThrow<HalJournalCausalAuditSession>();
  if (!audit) return failure("OOM: journal audit");
  bool found = false;
  if (!audit->run() || !audit->containsTintaCourse(course, found)) return failure("journal authority");
  return !found || failure("course history requires canonical recovery");
}
}  // namespace companion
