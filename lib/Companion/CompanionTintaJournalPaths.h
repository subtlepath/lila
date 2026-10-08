#pragma once

namespace companion {
inline constexpr char TINTA_JOURNAL_DIRECTORY[] = "/.crosspoint/companion/tinta-events";
inline constexpr char TINTA_JOURNAL_EVENTS[] = "/.crosspoint/companion/tinta-events/events.bin";
inline constexpr char TINTA_JOURNAL_HEADER_A[] = "/.crosspoint/companion/tinta-events/header-a.bin";
inline constexpr char TINTA_JOURNAL_HEADER_B[] = "/.crosspoint/companion/tinta-events/header-b.bin";
struct TintaJournalPaths {
  const char* directory;
  const char* events;
  const char* headerA;
  const char* headerB;
};
inline constexpr TintaJournalPaths ACTIVE_TINTA_JOURNAL_PATHS{TINTA_JOURNAL_DIRECTORY, TINTA_JOURNAL_EVENTS,
                                                              TINTA_JOURNAL_HEADER_A, TINTA_JOURNAL_HEADER_B};
inline constexpr TintaJournalPaths MIGRATION_TINTA_JOURNAL_PATHS{
    "/.crosspoint/companion/tinta-events-next", "/.crosspoint/companion/tinta-events-next/events.bin",
    "/.crosspoint/companion/tinta-events-next/header-a.bin", "/.crosspoint/companion/tinta-events-next/header-b.bin"};
inline constexpr TintaJournalPaths BACKUP_TINTA_JOURNAL_PATHS{
    "/.crosspoint/companion/tinta-events-old", "/.crosspoint/companion/tinta-events-old/events.bin",
    "/.crosspoint/companion/tinta-events-old/header-a.bin", "/.crosspoint/companion/tinta-events-old/header-b.bin"};
inline constexpr TintaJournalPaths MERGE_TINTA_JOURNAL_PATHS{
    "/.crosspoint/companion/tinta-events-merge", "/.crosspoint/companion/tinta-events-merge/events.bin",
    "/.crosspoint/companion/tinta-events-merge/header-a.bin", "/.crosspoint/companion/tinta-events-merge/header-b.bin"};
inline constexpr TintaJournalPaths MERGE_BACKUP_TINTA_JOURNAL_PATHS{
    "/.crosspoint/companion/tinta-events-previous", "/.crosspoint/companion/tinta-events-previous/events.bin",
    "/.crosspoint/companion/tinta-events-previous/header-a.bin",
    "/.crosspoint/companion/tinta-events-previous/header-b.bin"};
enum class TintaJournalLocation { Active, MigrationCandidate, Backup, MergeCandidate, MergeBackup };
inline const TintaJournalPaths* tintaJournalPaths(TintaJournalLocation location) {
  switch (location) {
    case TintaJournalLocation::Active:
      return &ACTIVE_TINTA_JOURNAL_PATHS;
    case TintaJournalLocation::Backup:
      return &BACKUP_TINTA_JOURNAL_PATHS;
    case TintaJournalLocation::MigrationCandidate:
      return &MIGRATION_TINTA_JOURNAL_PATHS;
    case TintaJournalLocation::MergeCandidate:
      return &MERGE_TINTA_JOURNAL_PATHS;
    case TintaJournalLocation::MergeBackup:
      return &MERGE_BACKUP_TINTA_JOURNAL_PATHS;
  }
  return nullptr;
}
}  // namespace companion
