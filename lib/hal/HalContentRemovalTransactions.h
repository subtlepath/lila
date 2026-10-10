#pragma once

#include "CompanionContentRemoval.h"
#include "HalCompletedRemovalJournalRelease.h"

namespace companion {
// Generation is measured/provisioned by the caller before admitting commands.
// Peer authorization and content-plan binding precede remove(). All logical
// journal/receipt/content writers remain serialized throughout each operation.
class HalContentRemovalTransactions final {
 public:
  HalContentRemovalTransactions(const Identity& generation, ContentRemovalJournal& journal,
                                HalContentRemovalJournalStorage& storage, HalCompletedContentRemovals& completions,
                                ContentRemovalParticipant& participant, std::span<uint8_t> releaseScratch)
      : generation(generation),
        journal(journal),
        completions(completions),
        removal(journal, participant),
        release(journal, storage, completions, releaseScratch) {}
  // Call before resolving a live inventory path; completed content may be gone.
  CompletedRemovalResult lookup(const ContentRemovalRequest& request, ContentRemovalRecord& output) {
    if (!validContentRemovalRequest(request)) return CompletedRemovalResult::Invalid;
    if (request.generation != generation) return CompletedRemovalResult::Conflict;
    expected.request = request;
    return completions.load(request, output);
  }
  // Call after successful removal/retry while the session still excludes writers.
  ContentRemovalJournalResult finishCompleted() {
    if (!validContentRemovalRequest(expected.request)) return ContentRemovalJournalResult::Invalid;
    const auto completed = completions.load(expected.request, receipt);
    if (completed != CompletedRemovalResult::Ok) return map(completed);
    const auto recovered = journal.recover(generation);
    if (recovered == ContentRemovalJournalResult::Missing) return ContentRemovalJournalResult::Ok;
    if (recovered != ContentRemovalJournalResult::Ok) return recovered;
    if (!journal.current() || journal.current()->request != expected.request)
      return ContentRemovalJournalResult::Conflict;
    return map(release.release());
  }
  // Release only a verified completed previous owner before live plan admission.
  ContentRemovalJournalResult prepare(const ContentRemovalRequest& request) {
    if (!validContentRemovalRequest(request)) return ContentRemovalJournalResult::Invalid;
    if (request.generation != generation) return ContentRemovalJournalResult::Conflict;
    expected.request = request;
    const auto recovered = journal.recover(generation);
    if (recovered == ContentRemovalJournalResult::Missing) return ContentRemovalJournalResult::Ok;
    if (recovered != ContentRemovalJournalResult::Ok) return recovered;
    checkpoint = *journal.current();
    if (checkpoint.request == expected.request) return ContentRemovalJournalResult::Ok;
    auto completed = completions.load(checkpoint.request, receipt);
    if (!unchanged()) return ContentRemovalJournalResult::Conflict;
    if (completed == CompletedRemovalResult::Missing && checkpoint.phase == ContentRemovalPhase::Retired)
      completed = completions.persist(checkpoint, journal);
    if (completed == CompletedRemovalResult::Missing) return ContentRemovalJournalResult::Conflict;
    if (completed != CompletedRemovalResult::Ok) return map(completed);
    if (!unchanged()) return ContentRemovalJournalResult::Conflict;
    return map(release.release());
  }
  ContentRemovalJournalResult remove(const ContentRemovalRecord& initial) {
    if (!validContentRemovalRecord(initial) || initial.phase != ContentRemovalPhase::Prepared)
      return ContentRemovalJournalResult::Invalid;
    if (initial.request.generation != generation) return ContentRemovalJournalResult::Conflict;
    expected = initial;
    auto completed = lookup(expected.request, receipt);
    if (completed == CompletedRemovalResult::Ok) return ContentRemovalJournalResult::Ok;
    if (completed != CompletedRemovalResult::Missing) return map(completed);
    const auto recovered = journal.recover(generation);
    if (recovered != ContentRemovalJournalResult::Missing && recovered != ContentRemovalJournalResult::Ok)
      return recovered;
    if (recovered == ContentRemovalJournalResult::Ok) {
      checkpoint = *journal.current();
      if (checkpoint.request == expected.request) {
        if (checkpoint.planHash != expected.planHash) return ContentRemovalJournalResult::Conflict;
        // Retired is authoritative even when later reading changes metadata.
        if (checkpoint.phase == ContentRemovalPhase::Retired) return map(completions.persist(checkpoint, journal));
      } else {
        completed = completions.load(checkpoint.request, receipt);
        if (!unchanged()) return ContentRemovalJournalResult::Conflict;
        if (completed == CompletedRemovalResult::Missing && checkpoint.phase == ContentRemovalPhase::Retired)
          completed = completions.persist(checkpoint, journal);
        if (completed == CompletedRemovalResult::Missing) return ContentRemovalJournalResult::Conflict;
        if (completed != CompletedRemovalResult::Ok) return map(completed);
        if (!unchanged()) return ContentRemovalJournalResult::Conflict;
        completed = release.release();
        if (completed != CompletedRemovalResult::Ok) return map(completed);
      }
    }
    const auto result = removal.remove(expected);
    if (result != ContentRemovalJournalResult::Ok) return result;
    if (!journal.current() || journal.current()->request != expected.request ||
        journal.current()->planHash != expected.planHash || journal.current()->phase != ContentRemovalPhase::Retired)
      return ContentRemovalJournalResult::Conflict;
    checkpoint = *journal.current();
    return map(completions.persist(checkpoint, journal));
  }

 private:
  Identity generation;
  ContentRemovalJournal& journal;
  HalCompletedContentRemovals& completions;
  ContentRemoval removal;
  HalCompletedRemovalJournalRelease release;
  ContentRemovalRecord expected, checkpoint, receipt;
  bool unchanged() const { return journal.current() && *journal.current() == checkpoint; }
  static ContentRemovalJournalResult map(CompletedRemovalResult result) {
    switch (result) {
      case CompletedRemovalResult::Ok:
        return ContentRemovalJournalResult::Ok;
      case CompletedRemovalResult::Missing:
        return ContentRemovalJournalResult::Missing;
      case CompletedRemovalResult::Invalid:
        return ContentRemovalJournalResult::Invalid;
      case CompletedRemovalResult::Conflict:
        return ContentRemovalJournalResult::Conflict;
      case CompletedRemovalResult::Corrupt:
        return ContentRemovalJournalResult::Corrupt;
      case CompletedRemovalResult::IoError:
        return ContentRemovalJournalResult::IoError;
    }
    return ContentRemovalJournalResult::Invalid;
  }
};
}  // namespace companion
