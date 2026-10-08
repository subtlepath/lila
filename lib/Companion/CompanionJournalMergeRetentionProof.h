#pragma once

#include "CompanionTintaJournal.h"

namespace companion {
// Candidate construction copies the active journal verbatim, then appends merged events.
// Caller audits both journals, excludes writers and owns this workspace outside the task stack.
class JournalMergeRetentionProof {
 public:
  TintaJournalResult verify(TintaJournal& previous, TintaJournal& candidate) {
    if (&previous == &candidate || !previous.available() || !candidate.available())
      return TintaJournalResult::Unavailable;
    const auto previousCount = previous.count(), candidateCount = candidate.count();
    if (candidate.recordSize() != TintaJournal::EXTENDED_RECORD_SIZE || candidateCount < previousCount)
      return TintaJournalResult::Conflict;
    for (uint32_t at = 0; at < previousCount; ++at) {
      if (previous.count() != previousCount || candidate.count() != candidateCount) return TintaJournalResult::Conflict;
      auto result = previous.read(at);
      if (result != TintaJournalResult::Ok) return result;
      event = previous.event();
      const auto body = previous.body();
      if (body.size() > bytes.size()) return TintaJournalResult::Corrupt;
      const size_t length = body.size();
      std::copy(body.begin(), body.end(), bytes.begin());
      result = candidate.read(at);
      if (result != TintaJournalResult::Ok) return result;
      if (!TintaJournal::sameEvent(event, candidate.event()) || candidate.body().size() != length ||
          !std::equal(candidate.body().begin(), candidate.body().end(), bytes.begin()))
        return TintaJournalResult::Conflict;
    }
    return previous.count() == previousCount && candidate.count() == candidateCount ? TintaJournalResult::Ok
                                                                                    : TintaJournalResult::Conflict;
  }

 private:
  SyncEvent event;
  std::array<uint8_t, TintaJournal::MAX_BODY_SIZE> bytes{};
};
}  // namespace companion
