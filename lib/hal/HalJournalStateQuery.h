#pragma once

#include "CompanionJournalState.h"
#include "HalJournalCausalAuditSession.h"

namespace companion {
// Caller authenticates the transport and excludes journal writers/export owners.
inline size_t handleJournalStateQuery(const Identity& generation, std::span<const uint8_t> request,
                                      std::span<uint8_t> reply) {
  Identity requested{};
  if (reply.size() < JOURNAL_STATE_REPLY_SIZE || !decodeJournalStateRequest(request, requested) ||
      requested != generation)
    return 0;
  // Audit handles and scratch exceed the task stack budget; release after this query.
  auto audit = makeUniqueNoThrow<HalJournalCausalAuditSession>();
  if (!audit) {
    LOG_ERR("COMPANION", "OOM: journal state audit");
    return 0;
  }
  JournalMergeSnapshot snapshot;
  if (!audit->run(&snapshot.frontier)) return 0;
  snapshot.count = audit->recordCount();
  snapshot.recordSize = audit->recordSize();
  return encodeJournalStateReply(snapshot, reply.first(JOURNAL_STATE_REPLY_SIZE)) ? JOURNAL_STATE_REPLY_SIZE : 0;
}
}  // namespace companion
