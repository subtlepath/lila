#pragma once

#include "CompanionContentRemovalJournal.h"

namespace companion {
class ContentRemovalParticipant {
 public:
  virtual ~ContentRemovalParticipant() = default;
  // Read-only verification of installed bytes, paths and recoverable backups.
  virtual bool verifyPlan(const ContentRemovalRecord& record) = 0;
  // Idempotent operations must inspect ownership before replacing/removing files.
  virtual bool quarantine(const ContentRemovalRecord& record) = 0;
  virtual bool verifyQuarantined(const ContentRemovalRecord& record) = 0;
  // Remove live references while retaining quarantined bytes and learner history.
  virtual bool publishRemoval(const ContentRemovalRecord& record) = 0;
  virtual bool verifyPublished(const ContentRemovalRecord& record) = 0;
  virtual bool retireBackups(const ContentRemovalRecord& record) = 0;
  virtual bool verifyRetired(const ContentRemovalRecord& record) = 0;
};
// Session-owned outside the task stack. The caller authorizes the request and
// excludes content mutations for the operation.
// The journal remains available after retirement for repeated-command recovery.
class ContentRemoval final {
 public:
  ContentRemoval(ContentRemovalJournal& journal, ContentRemovalParticipant& participant)
      : journal(journal), participant(participant) {}
  ContentRemovalJournalResult remove(const ContentRemovalRecord& initial) {
    if (initial.phase != ContentRemovalPhase::Prepared || !validContentRemovalRecord(initial))
      return ContentRemovalJournalResult::Invalid;
    expected = initial;
    auto result = journal.recover(expected);
    if (result == ContentRemovalJournalResult::Missing) {
      if (!participant.verifyPlan(expected)) return ContentRemovalJournalResult::Conflict;
      result = journal.begin(expected);
    } else if (result == ContentRemovalJournalResult::Ok && journal.current()->phase != ContentRemovalPhase::Retired) {
      result = journal.confirmRecovered();
    }
    if (result != ContentRemovalJournalResult::Ok) return result;
    while (const auto* current = journal.current()) {
      checkpoint = *current;
      switch (checkpoint.phase) {
        case ContentRemovalPhase::Prepared:
          if (!participant.verifyPlan(checkpoint) || !unchanged()) return ContentRemovalJournalResult::Conflict;
          if (!participant.quarantine(checkpoint)) return ContentRemovalJournalResult::IoError;
          if (!unchanged()) return ContentRemovalJournalResult::Conflict;
          if (!participant.verifyQuarantined(checkpoint)) return ContentRemovalJournalResult::IoError;
          break;
        case ContentRemovalPhase::Quarantined:
          if (!participant.verifyQuarantined(checkpoint) || !unchanged()) return ContentRemovalJournalResult::Conflict;
          if (!participant.publishRemoval(checkpoint)) return ContentRemovalJournalResult::IoError;
          if (!unchanged()) return ContentRemovalJournalResult::Conflict;
          if (!participant.verifyPublished(checkpoint)) return ContentRemovalJournalResult::IoError;
          break;
        case ContentRemovalPhase::Committed:
          if (!participant.verifyPublished(checkpoint) || !unchanged()) return ContentRemovalJournalResult::Conflict;
          if (!participant.retireBackups(checkpoint)) return ContentRemovalJournalResult::IoError;
          if (!unchanged()) return ContentRemovalJournalResult::Conflict;
          if (!participant.verifyRetired(checkpoint)) return ContentRemovalJournalResult::IoError;
          break;
        case ContentRemovalPhase::Retired:
          return participant.verifyRetired(checkpoint) && unchanged() ? ContentRemovalJournalResult::Ok
                                                                      : ContentRemovalJournalResult::Conflict;
      }
      if (!unchanged()) return ContentRemovalJournalResult::Conflict;
      result = journal.advance(static_cast<ContentRemovalPhase>(static_cast<unsigned>(checkpoint.phase) + 1));
      if (result != ContentRemovalJournalResult::Ok) return result;
    }
    return ContentRemovalJournalResult::Invalid;
  }

 private:
  ContentRemovalJournal& journal;
  ContentRemovalParticipant& participant;
  ContentRemovalRecord expected, checkpoint;
  bool unchanged() const { return journal.current() && *journal.current() == checkpoint; }
};
}  // namespace companion
