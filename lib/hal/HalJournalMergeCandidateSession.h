#pragma once

#include "CompanionJournalIncomingCourseValidation.h"
#include "HalJournalCausalAuditSession.h"
#include "HalJournalMergePublicationStorage.h"

namespace companion {
// Checked heap session; caller authenticates owner and excludes all journal/publication writers.
class HalJournalMergeCandidateSession {
 public:
  TintaJournalResult begin(const JournalMergeIntent& declaration, const Identity& generation) {
    ready = false;
    if (!candidateStorage.close()) return failure("candidate close");
    if (!validJournalMergeIntent(declaration) || declaration.generation != generation)
      return TintaJournalResult::Invalid;
    expected = declaration;
    bindAbortReceipt(declaration.transaction);
    if (!Storage.ensureDirectoryExists(TRANSFER_DIRECTORY)) return failure("private directory");
    auto publication = makeUniqueNoThrow<HalJournalMergePublicationStorage>();
    if (!publication) return failure("OOM: publication workspace");
    const auto cancelled = aborting.load(current);
    if (cancelled == JournalMigrationPresence::IoError) return failure("abort checkpoint");
    if (cancelled == JournalMigrationPresence::Present) return TintaJournalResult::Conflict;
    const auto abortedReceipt = aborted.load(current);
    if (abortedReceipt == JournalMigrationPresence::IoError) return failure("aborted receipt");
    if (abortedReceipt == JournalMigrationPresence::Present) return TintaJournalResult::Conflict;
    const auto pending = publication->intent(current);
    if (pending == JournalMigrationPresence::IoError) return failure("publication intent");
    if (pending == JournalMigrationPresence::Present) {
      if (current != expected) return TintaJournalResult::Conflict;
      if (recoverJournalMerge(*publication, generation) != JournalMigrationPublicationResult::Complete ||
          !receiving.clear(expected))
        return failure("publication recovery");
      return TintaJournalResult::Duplicate;
    }
    const auto completed = receipts.load(current);
    if (completed == JournalMigrationPresence::IoError) return failure("completion receipt");
    if (completed == JournalMigrationPresence::Present && current == expected) {
      const auto checkpoint = receiving.load(current);
      if (checkpoint == JournalMigrationPresence::IoError) return failure("completed checkpoint lookup");
      if (checkpoint == JournalMigrationPresence::Present && current == expected && !receiving.clear(expected))
        return failure("completed checkpoint cleanup");
      return TintaJournalResult::Duplicate;
    }
    if (publication->directory(JournalMergeDirectory::Active) != JournalMigrationPresence::Present ||
        publication->directory(JournalMergeDirectory::Backup) != JournalMigrationPresence::Missing)
      return TintaJournalResult::Conflict;
    // Audit is also the source of the resumable exact-prefix copy; no second source audit is needed.
    auto source = makeUniqueNoThrow<HalJournalCausalAuditSession>();
    if (!source) return failure("OOM: source audit workspace");
    Digest frontier{};
    if (!source->run(&frontier) || source->recordCount() != expected.previous.count ||
        source->recordSize() != expected.previous.recordSize || frontier != expected.previous.frontier)
      return TintaJournalResult::Conflict;
    const auto checkpoint = receiving.load(current);
    if (checkpoint == JournalMigrationPresence::IoError) return failure("receiving checkpoint");
    if (checkpoint == JournalMigrationPresence::Present && current != expected) return TintaJournalResult::Conflict;
    if (checkpoint == JournalMigrationPresence::Missing) {
      if (publication->directory(JournalMergeDirectory::Candidate) != JournalMigrationPresence::Missing)
        return TintaJournalResult::Conflict;
      if (!receiving.persist(expected)) return failure("receiving authorization");
    }
    if (candidate.open() != TintaJournalResult::Ok || candidate.count() > expected.merged.count)
      return failure("candidate recovery/count");
    if (candidate.count() <= expected.previous.count) {
      if (!source->copyTo(candidate)) return failure("candidate prefix copy");
      source.reset();
    } else {
      source.reset();
      if (!publication->retainsPrevious(expected, false)) return failure("resumed candidate prefix");
    }
    ready = true;
    return TintaJournalResult::Ok;
  }
  TintaJournalResult append(const SyncEvent& event, std::span<const uint8_t> body) {
    if (!ready) return TintaJournalResult::Unavailable;
    const auto result = candidate.appendBounded(event, body, expected.merged.count);
    if (result == TintaJournalResult::IoError || result == TintaJournalResult::Unavailable) {
      ready = false;
      if (!candidateStorage.close()) failure("append close");
    }
    return result;
  }
  uint32_t count() const { return ready ? candidate.count() : 0; }
  bool available() const { return ready && candidate.available(); }
  TintaJournalResult commit(const Identity& generation, const Identity* course, TintaSubjectCatalog* catalog) {
    if (!ready) return TintaJournalResult::Unavailable;
    if (generation != expected.generation || candidate.count() != expected.merged.count)
      return TintaJournalResult::Conflict;
    const auto membership = incoming.validate(candidate, expected.previous.count, course, catalog);
    if (membership != TintaJournalResult::Ok) {
      LOG_ERR("COMPANION", "Incoming journal course validation failed: %u", static_cast<unsigned>(membership));
      if (!candidate.available()) close();
      return membership;
    }
    ready = false;
    if (!candidateStorage.close()) return failure("commit close");
    {
      auto audit = makeUniqueNoThrow<HalJournalCausalAuditSession>(TintaJournalLocation::MergeCandidate);
      auto preferences = makeUniqueNoThrow<PortablePreferenceResolution>();
      if (!audit || !preferences) return failure("OOM: candidate preference audit");
      Digest frontier{};
      if (!audit->run(&frontier) || frontier != expected.merged.frontier ||
          audit->recordCount() != expected.merged.count || audit->recordSize() != expected.merged.recordSize)
        return failure("candidate preference authority");
      const auto resolved = audit->resolvePortablePreferences(*preferences);
      if (resolved != TintaJournalResult::Ok) {
        LOG_ERR("COMPANION", "Candidate preferences unresolved: %u", static_cast<unsigned>(resolved));
        return resolved;
      }
    }
    auto publication = makeUniqueNoThrow<HalJournalMergePublicationStorage>();
    if (!publication) return failure("OOM: publication workspace");
    if (!publication->authorize(expected, generation) ||
        recoverJournalMerge(*publication, generation) != JournalMigrationPublicationResult::Complete ||
        !receiving.clear(expected))
      return failure("candidate publication");
    return TintaJournalResult::Ok;
  }
  bool close() {
    ready = false;
    return candidateStorage.close();
  }
  TintaJournalResult abort(const JournalMergeIntent& declaration, const Identity& generation) {
    if (!validJournalMergeIntent(declaration) || declaration.generation != generation)
      return TintaJournalResult::Invalid;
    bindAbortReceipt(declaration.transaction);
    auto publication = makeUniqueNoThrow<HalJournalMergePublicationStorage>();
    if (!publication) return failure("OOM: abort publication workspace");
    const auto pending = publication->intent(current);
    if (pending == JournalMigrationPresence::IoError) return failure("abort publication lookup");
    if (pending == JournalMigrationPresence::Present) return TintaJournalResult::Conflict;
    const auto terminal = aborted.load(current);
    if (terminal == JournalMigrationPresence::IoError) return failure("aborted receipt lookup");
    if (terminal == JournalMigrationPresence::Present) {
      if (current != declaration) return TintaJournalResult::Conflict;
      return cleanupAborted(declaration) ? TintaJournalResult::Ok : failure("terminal abort cleanup");
    }
    const auto completed = receipts.load(current);
    if (completed == JournalMigrationPresence::IoError) return failure("abort receipt lookup");
    if (completed == JournalMigrationPresence::Present && current == declaration) return TintaJournalResult::Conflict;
    const auto cancelled = aborting.load(current);
    if (cancelled == JournalMigrationPresence::IoError) return failure("abort checkpoint lookup");
    if (cancelled == JournalMigrationPresence::Present && current != declaration) return TintaJournalResult::Conflict;
    const auto checkpoint = receiving.load(current);
    if (checkpoint == JournalMigrationPresence::IoError) return failure("abort receiving lookup");
    if (checkpoint == JournalMigrationPresence::Present && current != declaration) return TintaJournalResult::Conflict;
    if (checkpoint == JournalMigrationPresence::Missing && cancelled == JournalMigrationPresence::Missing) {
      const auto presence = publication->directory(JournalMergeDirectory::Candidate);
      if (presence == JournalMigrationPresence::IoError) return failure("queued abort candidate lookup");
      if (presence != JournalMigrationPresence::Missing) return TintaJournalResult::Conflict;
      if (!close() || !Storage.ensureDirectoryExists(TRANSFER_DIRECTORY) || !aborted.persist(declaration))
        return failure("queued abort receipt");
      return TintaJournalResult::Ok;
    }
    ready = false;
    if (!aborting.persist(declaration)) return failure("abort authorization");
    if (!close()) return failure("abort candidate close");
    const auto candidatePresence = publication->directory(JournalMergeDirectory::Candidate);
    if (candidatePresence == JournalMigrationPresence::IoError) return failure("abort candidate lookup");
    if (candidatePresence == JournalMigrationPresence::Present) {
      children.emplace(nullptr, nullptr, MERGE_TINTA_JOURNAL_PATHS.directory);
      for (const auto* path :
           {MERGE_TINTA_JOURNAL_PATHS.events, MERGE_TINTA_JOURNAL_PATHS.headerA, MERGE_TINTA_JOURNAL_PATHS.headerB}) {
        const auto presence = children->inspect(path);
        if (presence == CompanionFilePresence::Error) return failure("abort child lookup");
        if (presence == CompanionFilePresence::Present && !Storage.remove(path)) return failure("abort child remove");
      }
      children.reset();
      // Empty-directory removal preserves any unexpected files instead of recursively deleting them.
      if (!Storage.rmdir(MERGE_TINTA_JOURNAL_PATHS.directory)) return failure("abort directory remove");
    }
    if (!aborted.persist(declaration) || !receiving.clear(declaration) || !aborting.clear(declaration))
      return failure("abort receipt/checkpoint cleanup");
    return TintaJournalResult::Ok;
  }

 private:
  bool cleanupAborted(const JournalMergeIntent& declaration) {
    for (auto* record : {&receiving, &aborting}) {
      const auto presence = record->load(current);
      if (presence == JournalMigrationPresence::IoError) return false;
      if (presence == JournalMigrationPresence::Present && current == declaration && !record->clear(declaration))
        return false;
    }
    return true;
  }
  void bindAbortReceipt(const Identity& transaction) {
    static constexpr char PREFIX[] = "/.crosspoint/companion/journal-merge-aborted-";
    static constexpr char HEX_DIGITS[] = "0123456789abcdef";
    static constexpr size_t PREFIX_SIZE = sizeof(PREFIX) - 1;
    static_assert(PREFIX_SIZE + 32 + 6 <= 96);
    std::copy_n(PREFIX, PREFIX_SIZE, abortedPath.begin());
    for (size_t at = 0; at < transaction.size(); ++at) {
      abortedPath[PREFIX_SIZE + 2 * at] = HEX_DIGITS[transaction[at] >> 4];
      abortedPath[PREFIX_SIZE + 2 * at + 1] = HEX_DIGITS[transaction[at] & 15];
    }
    abortedPath[PREFIX_SIZE + 32] = 0;
    std::copy_n(abortedPath.begin(), PREFIX_SIZE + 32, abortedStage.begin());
    std::copy_n("-next", 6, abortedStage.begin() + PREFIX_SIZE + 32);
  }
  static TintaJournalResult failure(const char* reason) {
    LOG_ERR("COMPANION", "Journal merge candidate failed: %s", reason);
    return TintaJournalResult::IoError;
  }
  HalJournalMergeRecordStore receiving{JournalMergeRecord::Receiving}, receipts{JournalMergeRecord::Receipt};
  HalJournalMergeRecordStore aborting{JournalMergeRecord::Aborting};
  std::array<char, 96> abortedPath{}, abortedStage{};
  HalJournalMergeRecordStore aborted{abortedPath.data(), abortedStage.data()};
  std::optional<HalCompanionFileLookup> children;
  std::array<uint8_t, TintaJournal::EXTENDED_RECORD_SIZE> scratch{};
  HalTintaJournalStorage candidateStorage{TintaJournalLocation::MergeCandidate};
  TintaJournal candidate{candidateStorage, scratch};
  JournalMergeIntent expected, current;
  JournalIncomingCourseValidation incoming;
  bool ready = false;
};
// Companion startup supplies the provisioned generation before enabling radios.
inline bool recoverExistingJournalMergeAbort(const Identity& generation) {
  struct Recovery {
    HalJournalMergeRecordStore aborting{JournalMergeRecord::Aborting};
    JournalMergeIntent pending;
    HalJournalMergeCandidateSession candidate;
  };
  // Record buffers and journal handles exceed the task stack budget.
  auto recovery = makeUniqueNoThrow<Recovery>();
  if (!recovery) {
    LOG_ERR("COMPANION", "OOM: journal abort recovery workspace");
    return false;
  }
  const auto presence = recovery->aborting.load(recovery->pending);
  if (presence == JournalMigrationPresence::Missing) return true;
  if (presence != JournalMigrationPresence::Present || recovery->pending.generation != generation ||
      recovery->candidate.abort(recovery->pending, generation) != TintaJournalResult::Ok) {
    LOG_ERR("COMPANION", "Journal abort startup recovery failed");
    return false;
  }
  return true;
}
}  // namespace companion
