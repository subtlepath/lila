#pragma once

#include "CompanionBookmarkPreparationClaim.h"

namespace companion {
enum class BookmarkPreparationRole : uint8_t { Identities, Bodies, Json };
enum class BookmarkPreparationFile : uint8_t { Missing, Regular, Other, Error };
class BookmarkPreparationStorage {
 public:
  virtual ~BookmarkPreparationStorage() = default;
  virtual bool validateContext(const BookmarkPreparationClaim&) = 0;
  virtual BookmarkPublicationRecord ownership(const BookmarkPreparationClaim&) = 0;
  // Any publication intent, including its temporary record, protects the spools.
  virtual BookmarkPublicationRecord publication() = 0;
  virtual BookmarkPreparationFile file(BookmarkPreparationRole) = 0;
  virtual bool persist(const BookmarkPreparationClaim&) = 0;
  virtual bool remove(BookmarkPreparationRole) = 0;
  virtual bool clear(const BookmarkPreparationClaim&) = 0;
};
// Caller excludes all spool/publication writers for the complete operation.
class BookmarkPreparation final {
 public:
  explicit BookmarkPreparation(BookmarkPreparationStorage& storage) : storage(storage) {}
  TintaJournalResult begin(const BookmarkPreparationClaim& claim) {
    if (!validBookmarkPreparationClaim(claim) || !storage.validateContext(claim)) return TintaJournalResult::Invalid;
    const auto owned = storage.ownership(claim);
    if (owned == BookmarkPublicationRecord::Error) return TintaJournalResult::IoError;
    if (owned != BookmarkPublicationRecord::Missing) return TintaJournalResult::Corrupt;
    const auto publication = withoutPublication();
    if (publication != TintaJournalResult::Ok) return publication;
    for (const auto role : ROLES) {
      const auto file = storage.file(role);
      if (file == BookmarkPreparationFile::Error) return TintaJournalResult::IoError;
      if (file != BookmarkPreparationFile::Missing) return TintaJournalResult::Corrupt;
    }
    if (!storage.validateContext(claim)) return TintaJournalResult::Invalid;
    if (!storage.persist(claim)) return TintaJournalResult::IoError;
    return guard(claim);
  }
  // Call before beginning any spool write; never infer ownership from filenames.
  TintaJournalResult guard(const BookmarkPreparationClaim& claim) {
    if (!validBookmarkPreparationClaim(claim) || !storage.validateContext(claim)) return TintaJournalResult::Invalid;
    const auto owned = storage.ownership(claim);
    if (owned == BookmarkPublicationRecord::Error) return TintaJournalResult::IoError;
    if (owned != BookmarkPublicationRecord::Matches) return TintaJournalResult::Corrupt;
    return withoutPublication();
  }
  TintaJournalResult recover(const BookmarkPreparationClaim& claim) {
    auto checked = guard(claim);
    if (checked != TintaJournalResult::Ok) return checked;
    // Preflight every role before removing any bytes.
    for (const auto role : ROLES) {
      const auto file = storage.file(role);
      if (file == BookmarkPreparationFile::Error) return TintaJournalResult::IoError;
      if (file == BookmarkPreparationFile::Other) return TintaJournalResult::Corrupt;
    }
    for (const auto role : ROLES) {
      checked = guard(claim);
      if (checked != TintaJournalResult::Ok) return checked;
      const auto file = storage.file(role);
      if (file == BookmarkPreparationFile::Error) return TintaJournalResult::IoError;
      if (file == BookmarkPreparationFile::Other) return TintaJournalResult::Corrupt;
      if (file == BookmarkPreparationFile::Regular && !storage.remove(role)) return TintaJournalResult::IoError;
    }
    checked = guard(claim);
    if (checked != TintaJournalResult::Ok) return checked;
    return storage.clear(claim) ? TintaJournalResult::Ok : TintaJournalResult::IoError;
  }

 private:
  TintaJournalResult withoutPublication() {
    const auto record = storage.publication();
    if (record == BookmarkPublicationRecord::Error) return TintaJournalResult::IoError;
    return record == BookmarkPublicationRecord::Missing ? TintaJournalResult::Ok : TintaJournalResult::Conflict;
  }
  static constexpr BookmarkPreparationRole ROLES[] = {BookmarkPreparationRole::Identities,
                                                      BookmarkPreparationRole::Bodies, BookmarkPreparationRole::Json};
  BookmarkPreparationStorage& storage;
};
}  // namespace companion
