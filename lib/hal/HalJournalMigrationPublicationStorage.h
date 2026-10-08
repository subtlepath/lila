#pragma once

#include "HalJournalCausalAuditSession.h"
#include "HalJournalMigrationIntentStore.h"

namespace companion {
// Session-owned storage. Writers are excluded; audit workspaces die before rename.
class HalJournalMigrationPublicationStorage final : public JournalMigrationPublicationStorage {
 public:
  JournalMigrationPresence intent(JournalMigrationIntent& output) override {
    const auto result = intents.load(output);
    if (result == JournalMigrationPresence::Present) authorization = output;
    return result;
  }
  JournalMigrationPresence directory(JournalMigrationDirectory location) override {
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
  bool verify(JournalMigrationDirectory location, const JournalMigrationIntent& expected,
              uint16_t recordSize) override {
    const auto* paths = pathsFor(location);
    if (!paths || directory(location) != JournalMigrationPresence::Present) return failure("verification directory");
    children.emplace(nullptr, nullptr, paths->directory);
    const auto events = children->inspect(paths->events);
    const auto a = children->inspect(paths->headerA), b = children->inspect(paths->headerB);
    children.reset();
    if (events != CompanionFilePresence::Present || a == CompanionFilePresence::Error ||
        b == CompanionFilePresence::Error ||
        (a != CompanionFilePresence::Present && b != CompanionFilePresence::Present))
      return failure("missing journal files");
    // Buffers/retained handles exceed the task stack; allocate once per verification.
    auto audit = makeUniqueNoThrow<HalJournalCausalAuditSession>(locationFor(location));
    if (!audit) return failure("OOM: audit workspace");
    Digest frontier;
    if (!audit->run(&frontier) || audit->recordCount() != expected.count || audit->recordSize() != recordSize ||
        frontier != expected.frontier)
      return failure("journal binding");
    return true;
  }
  bool move(JournalMigrationDirectory from, JournalMigrationDirectory to) override {
    const auto* source = pathsFor(from);
    const auto* target = pathsFor(to);
    if (!source || !target || from == to || directory(from) != JournalMigrationPresence::Present ||
        directory(to) != JournalMigrationPresence::Missing)
      return failure("rename ownership");
    return Storage.rename(source->directory, target->directory) || failure("directory rename");
  }
  bool authorize(const JournalMigrationIntent& expected) {
    if (directory(JournalMigrationDirectory::Backup) != JournalMigrationPresence::Missing ||
        !verify(JournalMigrationDirectory::Active, expected, 512) ||
        !verify(JournalMigrationDirectory::Candidate, expected, 1024))
      return failure("authorization verification");
    return intents.persist(expected);
  }
  bool clearIntent() override { return intents.clear(authorization); }

 private:
  static TintaJournalLocation locationFor(JournalMigrationDirectory location) {
    switch (location) {
      case JournalMigrationDirectory::Active:
        return TintaJournalLocation::Active;
      case JournalMigrationDirectory::Candidate:
        return TintaJournalLocation::MigrationCandidate;
      case JournalMigrationDirectory::Backup:
        return TintaJournalLocation::Backup;
    }
    return static_cast<TintaJournalLocation>(255);
  }
  static const TintaJournalPaths* pathsFor(JournalMigrationDirectory location) {
    return tintaJournalPaths(locationFor(location));
  }
  static bool failure(const char* reason) {
    LOG_ERR("COMPANION", "Journal migration publication failed: %s", reason);
    return false;
  }
  HalJournalMigrationIntentStore intents;
  HalCompanionFileLookup lookup;
  std::optional<HalCompanionFileLookup> children;
  JournalMigrationIntent authorization;
};
// Lookup belongs to the caller's startup workspace, outside the task stack.
inline bool recoverExistingJournalMigration(HalCompanionFileLookup& lookup) {
  if (!Storage.ensureDirectoryExists(TRANSFER_DIRECTORY)) {
    LOG_ERR("COMPANION", "Cannot prepare journal migration lookup");
    return false;
  }
  const auto presence = lookup.inspect(HalJournalMigrationIntentStore::PATH);
  if (presence == CompanionFilePresence::Missing) return true;
  if (presence == CompanionFilePresence::Error) return false;
  // Provider owns several retained handles and lookup buffers; avoid the task stack.
  auto publication = makeUniqueNoThrow<HalJournalMigrationPublicationStorage>();
  if (!publication) {
    LOG_ERR("COMPANION", "OOM: journal migration recovery workspace");
    return false;
  }
  const auto result = recoverJournalMigration(*publication);
  if (result == JournalMigrationPublicationResult::Complete || result == JournalMigrationPublicationResult::NoPending)
    return true;
  LOG_ERR("COMPANION", "Journal migration recovery failed: %u", static_cast<unsigned>(result));
  return false;
}
}  // namespace companion
