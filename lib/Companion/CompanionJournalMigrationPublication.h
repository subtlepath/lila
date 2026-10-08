#pragma once

#include "CompanionRecords.h"

namespace companion {
enum class JournalMigrationPresence { Missing, Present, IoError };
enum class JournalMigrationDirectory { Active, Candidate, Backup };
enum class JournalMigrationPublicationResult { NoPending, Complete, Conflict, IoError };
struct JournalMigrationIntent {
  uint32_t count = 0;
  Digest frontier{};
};
class JournalMigrationPublicationStorage {
 public:
  virtual ~JournalMigrationPublicationStorage() = default;
  // Only a durable intent written after exact copy verification authorizes publication.
  virtual JournalMigrationPresence intent(JournalMigrationIntent& output) = 0;
  virtual JournalMigrationPresence directory(JournalMigrationDirectory location) = 0;
  // Reopen and audit the journal, checking count, record size, and global frontier. No writer may run.
  virtual bool verify(JournalMigrationDirectory location, const JournalMigrationIntent& intent,
                      uint16_t recordSize) = 0;
  // Rename whole directories, with all file handles closed. False may mean lost acknowledgement.
  virtual bool move(JournalMigrationDirectory from, JournalMigrationDirectory to) = 0;
  virtual bool clearIntent() = 0;
};
// Backup is retained. Caller excludes all journal writers until this succeeds.
inline JournalMigrationPublicationResult recoverJournalMigration(JournalMigrationPublicationStorage& storage) {
  JournalMigrationIntent intent;
  const auto pending = storage.intent(intent);
  if (pending == JournalMigrationPresence::IoError) return JournalMigrationPublicationResult::IoError;
  if (pending == JournalMigrationPresence::Missing) return JournalMigrationPublicationResult::NoPending;
  bool nonzero = false;
  for (const auto byte : intent.frontier) nonzero |= byte != 0;
  if (!nonzero || intent.count > UINT32_MAX / 1024) return JournalMigrationPublicationResult::Conflict;
  const auto active = storage.directory(JournalMigrationDirectory::Active);
  const auto candidate = storage.directory(JournalMigrationDirectory::Candidate);
  const auto backup = storage.directory(JournalMigrationDirectory::Backup);
  if (active == JournalMigrationPresence::IoError || candidate == JournalMigrationPresence::IoError ||
      backup == JournalMigrationPresence::IoError)
    return JournalMigrationPublicationResult::IoError;
  const bool a = active == JournalMigrationPresence::Present;
  const bool c = candidate == JournalMigrationPresence::Present;
  const bool b = backup == JournalMigrationPresence::Present;
  if (a && c && !b) {
    if (!storage.verify(JournalMigrationDirectory::Active, intent, 512) ||
        !storage.verify(JournalMigrationDirectory::Candidate, intent, 1024))
      return JournalMigrationPublicationResult::Conflict;
    if (!storage.move(JournalMigrationDirectory::Active, JournalMigrationDirectory::Backup))
      return JournalMigrationPublicationResult::IoError;
  } else if (!a && b && c) {
    if (!storage.verify(JournalMigrationDirectory::Backup, intent, 512) ||
        !storage.verify(JournalMigrationDirectory::Candidate, intent, 1024))
      return JournalMigrationPublicationResult::Conflict;
  } else if (a && b && !c) {
    if (!storage.verify(JournalMigrationDirectory::Active, intent, 1024) ||
        !storage.verify(JournalMigrationDirectory::Backup, intent, 512))
      return JournalMigrationPublicationResult::Conflict;
    return storage.clearIntent() ? JournalMigrationPublicationResult::Complete
                                 : JournalMigrationPublicationResult::IoError;
  } else {
    return JournalMigrationPublicationResult::Conflict;
  }
  if (!storage.move(JournalMigrationDirectory::Candidate, JournalMigrationDirectory::Active))
    return JournalMigrationPublicationResult::IoError;
  // Reopen the published generation before retiring the authorization.
  if (!storage.verify(JournalMigrationDirectory::Active, intent, 1024))
    return JournalMigrationPublicationResult::Conflict;
  return storage.clearIntent() ? JournalMigrationPublicationResult::Complete
                               : JournalMigrationPublicationResult::IoError;
}
}  // namespace companion
