#pragma once

#include "CompanionJournalMergeRetentionProof.h"
#include "HalJournalCausalAuditSession.h"
#include "HalJournalMergeRecordStore.h"

namespace companion {
// Allocate outside the task stack; caller excludes journal and publication writers.
class HalJournalMergePublicationStorage final : public JournalMergePublicationStorage {
 public:
  JournalMigrationPresence intent(JournalMergeIntent& output) override { return intents.load(output); }
  JournalMigrationPresence directory(JournalMergeDirectory location) override {
    const auto* paths = pathsFor(location);
    if (!paths) return JournalMigrationPresence::IoError;
    const auto presence = lookup.inspect(paths->directory);
    if (presence == CompanionFilePresence::Missing) return JournalMigrationPresence::Missing;
    if (presence == CompanionFilePresence::Error) return JournalMigrationPresence::IoError;
    HalFile file;
    if (!Storage.openFileForRead("COMPANION", paths->directory, file) || !file.isDirectory()) {
      failure("directory type/open");
      return JournalMigrationPresence::IoError;
    }
    return JournalMigrationPresence::Present;
  }
  bool verify(JournalMergeDirectory location, const JournalMergeSnapshot& expected) override {
    const auto* paths = pathsFor(location);
    if (!paths || !validJournalMergeSnapshot(expected) || directory(location) != JournalMigrationPresence::Present)
      return failure("verification directory");
    children.emplace(nullptr, nullptr, paths->directory);
    const auto events = children->inspect(paths->events);
    const auto a = children->inspect(paths->headerA), b = children->inspect(paths->headerB);
    children.reset();
    if (events != CompanionFilePresence::Present || a == CompanionFilePresence::Error ||
        b == CompanionFilePresence::Error ||
        (a != CompanionFilePresence::Present && b != CompanionFilePresence::Present))
      return failure("missing journal files");
    // Full audit buffers exceed the stack budget and are released before rename.
    auto audit = makeUniqueNoThrow<HalJournalCausalAuditSession>(locationFor(location));
    if (!audit) return failure("OOM: audit workspace");
    Digest frontier{};
    if (!audit->run(&frontier) || audit->recordCount() != expected.count ||
        audit->recordSize() != expected.recordSize || frontier != expected.frontier)
      return failure("journal binding");
    return true;
  }
  bool move(JournalMergeDirectory from, JournalMergeDirectory to) override {
    const auto* source = pathsFor(from);
    const auto* target = pathsFor(to);
    if (!source || !target || from == to || directory(from) != JournalMigrationPresence::Present ||
        directory(to) != JournalMigrationPresence::Missing)
      return failure("rename ownership");
    return Storage.rename(source->directory, target->directory) || failure("directory rename");
  }
  bool authorize(const JournalMergeIntent& expected, const Identity& generation) {
    if (!validJournalMergeIntent(expected) || expected.generation != generation ||
        directory(JournalMergeDirectory::Backup) != JournalMigrationPresence::Missing ||
        !verify(JournalMergeDirectory::Active, expected.previous) ||
        !verify(JournalMergeDirectory::Candidate, expected.merged))
      return failure("authorization audit");
    if (!retainsPrevious(expected, true)) return false;
    return intents.persist(expected);
  }
  // Used for resuming a partial candidate after auditing the unchanged active snapshot.
  bool retainsPrevious(const JournalMergeIntent& expected, bool completeCandidate) {
    if (!validJournalMergeIntent(expected)) return failure("retention arguments");
    // Independent journal buffers and the saved envelope/body exceed the stack budget.
    auto prefix = makeUniqueNoThrow<PrefixWorkspace>();
    if (!prefix) return failure("OOM: retention workspace");
    const bool retained = prefix->previous.open() == TintaJournalResult::Ok &&
                          prefix->candidate.open() == TintaJournalResult::Ok &&
                          prefix->previous.count() == expected.previous.count &&
                          prefix->previous.recordSize() == expected.previous.recordSize &&
                          prefix->candidate.count() <= expected.merged.count &&
                          (!completeCandidate || prefix->candidate.count() == expected.merged.count) &&
                          prefix->candidate.recordSize() == expected.merged.recordSize &&
                          prefix->proof.verify(prefix->previous, prefix->candidate) == TintaJournalResult::Ok;
    const bool previousClosed = prefix->previousStorage.close();
    const bool candidateClosed = prefix->candidateStorage.close();
    prefix.reset();
    if (!retained || !previousClosed || !candidateClosed) return failure("retention proof/close");
    return true;
  }
  bool complete(const JournalMergeIntent& expected) override {
    const auto presence = intents.load(current);
    if (presence == JournalMigrationPresence::Missing)
      return (receipts.load(current) == JournalMigrationPresence::Present && current == expected) ||
             failure("completion receipt");
    if (presence != JournalMigrationPresence::Present || current != expected) return failure("completion intent");
    return receipts.persist(expected) && intents.clear(expected);
  }

 private:
  struct PrefixWorkspace {
    std::array<uint8_t, TintaJournal::EXTENDED_RECORD_SIZE> previousScratch{}, candidateScratch{};
    HalTintaJournalStorage previousStorage;
    HalTintaJournalStorage candidateStorage{TintaJournalLocation::MergeCandidate};
    TintaJournal previous{previousStorage, previousScratch}, candidate{candidateStorage, candidateScratch};
    JournalMergeRetentionProof proof;
  };
  static TintaJournalLocation locationFor(JournalMergeDirectory location) {
    switch (location) {
      case JournalMergeDirectory::Active:
        return TintaJournalLocation::Active;
      case JournalMergeDirectory::Candidate:
        return TintaJournalLocation::MergeCandidate;
      case JournalMergeDirectory::Backup:
        return TintaJournalLocation::MergeBackup;
    }
    return static_cast<TintaJournalLocation>(255);
  }
  static const TintaJournalPaths* pathsFor(JournalMergeDirectory location) {
    return tintaJournalPaths(locationFor(location));
  }
  static bool failure(const char* reason) {
    LOG_ERR("COMPANION", "Journal merge publication failed: %s", reason);
    return false;
  }
  HalJournalMergeRecordStore intents, receipts{JournalMergeRecord::Receipt};
  HalCompanionFileLookup lookup;
  std::optional<HalCompanionFileLookup> children;
  JournalMergeIntent current;
};
}  // namespace companion
