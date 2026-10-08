#pragma once

#include "CompanionJournalIdentityIndex.h"
#include "CompanionTintaJournalFrontier.h"

namespace companion {
// Caller first audits closure, freezes both files, and keeps all scratch banks disjoint.
inline TintaJournalResult streamJournalFrontier(TintaJournal& journal, IndexedJournalIdentities& index,
                                                TintaJournalFrontierEncoding& encoding) {
  if (!journal.available() || !index.available()) return TintaJournalResult::Unavailable;
  const auto count = journal.count();
  if (index.recordCount() != count) return TintaJournalResult::Corrupt;
  if (!encoding.begin(count)) return TintaJournalResult::Invalid;
  JournalIdentityEntry entry;
  for (uint32_t at = 0; at < count; ++at) {
    if (!index.read(at, entry)) return TintaJournalResult::IoError;
    const auto result = journal.read(entry.record);
    if (result != TintaJournalResult::Ok) return result;
    if (journal.event().identity != entry.identity) return TintaJournalResult::Corrupt;
    if (!encoding.append(journal.event())) return TintaJournalResult::Invalid;
  }
  if (journal.count() != count) return TintaJournalResult::Conflict;
  return encoding.complete() ? TintaJournalResult::Ok : TintaJournalResult::Invalid;
}
// The count is bound to a durable baseline checkpoint. Filtering the current
// identity index preserves canonical ordering even when new origins sort earlier.
inline TintaJournalResult streamJournalPrefixFrontier(TintaJournal& journal, IndexedJournalIdentities& index,
                                                      uint32_t prefixCount, TintaJournalFrontierEncoding& encoding) {
  if (!journal.available() || !index.available()) return TintaJournalResult::Unavailable;
  const auto count = journal.count();
  if (index.recordCount() != count || prefixCount > count) return TintaJournalResult::Corrupt;
  if (!encoding.begin(prefixCount)) return TintaJournalResult::Invalid;
  JournalIdentityEntry entry;
  for (uint32_t at = 0; at < count; ++at) {
    if (!index.read(at, entry)) return TintaJournalResult::IoError;
    if (entry.record >= count) return TintaJournalResult::Corrupt;
    if (entry.record >= prefixCount) continue;
    const auto result = journal.read(entry.record);
    if (result != TintaJournalResult::Ok) return result;
    if (journal.event().identity != entry.identity) return TintaJournalResult::Corrupt;
    if (!encoding.append(journal.event())) return TintaJournalResult::Invalid;
  }
  if (journal.count() != count) return TintaJournalResult::Conflict;
  return encoding.complete() ? TintaJournalResult::Ok : TintaJournalResult::Invalid;
}
}  // namespace companion
