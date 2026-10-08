#pragma once

#include "CompanionJournalMigrationPublication.h"
#include "CompanionTintaBody.h"

namespace companion {
struct JournalMergeSnapshot {
  uint32_t count = 0;
  uint16_t recordSize = 0;
  Digest frontier{};
  bool operator==(const JournalMergeSnapshot&) const = default;
};
struct JournalMergeIntent {
  Identity generation{}, transaction{}, owner{};
  JournalMergeSnapshot previous, merged;
  bool operator==(const JournalMergeIntent&) const = default;
};
static_assert(sizeof(JournalMergeIntent) < 256);
enum class JournalMergeDirectory : uint8_t { Active, Candidate, Backup };
class JournalMergePublicationStorage {
 public:
  virtual ~JournalMergePublicationStorage() = default;
  // Intent is durable only after auditing candidate closure and proving it retains every old event.
  virtual JournalMigrationPresence intent(JournalMergeIntent& output) = 0;
  virtual JournalMigrationPresence directory(JournalMergeDirectory location) = 0;
  // Audit count, record size, identities, causal closure, undo targets and global frontier.
  virtual bool verify(JournalMergeDirectory location, const JournalMergeSnapshot& expected) = 0;
  // All journal handles must be closed; false may mean the rename happened.
  virtual bool move(JournalMergeDirectory from, JournalMergeDirectory to) = 0;
  // Persist a transaction-bound completion receipt before retiring intent. Both operations are idempotent.
  virtual bool complete(const JournalMergeIntent& intent) = 0;
};
inline bool validJournalMergeSnapshot(const JournalMergeSnapshot& snapshot) {
  return (snapshot.recordSize == 512 || snapshot.recordSize == 1024) &&
         snapshot.count <= UINT32_MAX / snapshot.recordSize && tinta_body_detail::nonzero(snapshot.frontier);
}
inline bool validJournalMergeIntent(const JournalMergeIntent& intent) {
  return tinta_body_detail::nonzero(intent.generation) && tinta_body_detail::nonzero(intent.transaction) &&
         tinta_body_detail::nonzero(intent.owner) && validJournalMergeSnapshot(intent.previous) &&
         validJournalMergeSnapshot(intent.merged) && intent.merged.recordSize == 1024 &&
         intent.merged.count >= intent.previous.count &&
         (intent.merged.count != intent.previous.count || intent.merged.frontier == intent.previous.frontier);
}
// Caller excludes writers and uses a merge backup distinct from the legacy-format backup.
inline JournalMigrationPublicationResult recoverJournalMerge(JournalMergePublicationStorage& storage,
                                                             const Identity& currentGeneration) {
  JournalMergeIntent pending;
  const auto presence = storage.intent(pending);
  if (presence == JournalMigrationPresence::Missing) return JournalMigrationPublicationResult::NoPending;
  if (presence == JournalMigrationPresence::IoError) return JournalMigrationPublicationResult::IoError;
  if (!validJournalMergeIntent(pending) || pending.generation != currentGeneration)
    return JournalMigrationPublicationResult::Conflict;
  const auto active = storage.directory(JournalMergeDirectory::Active);
  const auto candidate = storage.directory(JournalMergeDirectory::Candidate);
  const auto backup = storage.directory(JournalMergeDirectory::Backup);
  if (active == JournalMigrationPresence::IoError || candidate == JournalMigrationPresence::IoError ||
      backup == JournalMigrationPresence::IoError)
    return JournalMigrationPublicationResult::IoError;
  const bool a = active == JournalMigrationPresence::Present;
  const bool c = candidate == JournalMigrationPresence::Present;
  const bool b = backup == JournalMigrationPresence::Present;
  if (a && c && !b) {
    if (!storage.verify(JournalMergeDirectory::Active, pending.previous) ||
        !storage.verify(JournalMergeDirectory::Candidate, pending.merged))
      return JournalMigrationPublicationResult::Conflict;
    if (!storage.move(JournalMergeDirectory::Active, JournalMergeDirectory::Backup))
      return JournalMigrationPublicationResult::IoError;
  } else if (!a && b && c) {
    if (!storage.verify(JournalMergeDirectory::Backup, pending.previous) ||
        !storage.verify(JournalMergeDirectory::Candidate, pending.merged))
      return JournalMigrationPublicationResult::Conflict;
  } else if (a && b && !c) {
    if (!storage.verify(JournalMergeDirectory::Backup, pending.previous) ||
        !storage.verify(JournalMergeDirectory::Active, pending.merged))
      return JournalMigrationPublicationResult::Conflict;
    return storage.complete(pending) ? JournalMigrationPublicationResult::Complete
                                     : JournalMigrationPublicationResult::IoError;
  } else {
    return JournalMigrationPublicationResult::Conflict;
  }
  if (!storage.move(JournalMergeDirectory::Candidate, JournalMergeDirectory::Active))
    return JournalMigrationPublicationResult::IoError;
  if (!storage.verify(JournalMergeDirectory::Active, pending.merged))
    return JournalMigrationPublicationResult::Conflict;
  return storage.complete(pending) ? JournalMigrationPublicationResult::Complete
                                   : JournalMigrationPublicationResult::IoError;
}
}  // namespace companion
