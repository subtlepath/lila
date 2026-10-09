#pragma once

#include "CompanionBookmarkPreparation.h"
#include "HalBookmarkBodyStage.h"
#include "HalBookmarkIdentityStage.h"
#include "HalBookmarkPreparationRecord.h"
#include "HalBookmarkPublicationPaths.h"

namespace companion {
// Checked off-stack owner. Caller excludes preparation/publication writers.
class HalBookmarkPreparationStorage final : public BookmarkPreparationStorage {
 public:
  using Validator = bool (*)(void*, const BookmarkPreparationClaim&);
  static constexpr char PATH[] = "/.crosspoint/companion/bookmark-preparation";
  static constexpr char NEXT[] = "/.crosspoint/companion/bookmark-preparation-next";
  HalBookmarkPreparationStorage(Validator validator, void* context) : validator(validator), context(context) {}
  bool validateContext(const BookmarkPreparationClaim& claim) override {
    bound = validBookmarkPreparationClaim(claim) && validator && validator(context, claim);
    if (bound) binding = claim;
    return bound;
  }
  BookmarkPublicationRecord ownership(const BookmarkPreparationClaim& claim) override {
    const auto active = records.inspect(PATH, claim);
    if (active == BookmarkPublicationRecord::Error || active == BookmarkPublicationRecord::Other) return active;
    const auto pending = records.inspect(NEXT, claim);
    if (pending == BookmarkPublicationRecord::Error || pending == BookmarkPublicationRecord::Other) return pending;
    // A temporary claim must be synced/promoted before authorizing spool writes.
    if (active == BookmarkPublicationRecord::Missing && pending == BookmarkPublicationRecord::Matches)
      return BookmarkPublicationRecord::Other;
    return active;
  }
  BookmarkPublicationRecord load(BookmarkPreparationClaim& output) {
    auto status = records.read(PATH, loaded);
    const bool temporary = status == BookmarkPublicationRecord::Missing;
    if (temporary) status = records.read(NEXT, loaded);
    if (status != BookmarkPublicationRecord::Matches) return status;
    if (!validateContext(loaded)) return BookmarkPublicationRecord::Other;
    if (temporary && !records.persist(PATH, NEXT, loaded)) return BookmarkPublicationRecord::Error;
    status = ownership(loaded);
    if (status != BookmarkPublicationRecord::Matches) return status;
    output = loaded;
    return BookmarkPublicationRecord::Matches;
  }
  TintaJournalResult validatePublication(const BookmarkPublicationClaim& publication) {
    auto status = records.read(PATH, loaded);
    if (status == BookmarkPublicationRecord::Missing) status = records.read(NEXT, loaded);
    if (status == BookmarkPublicationRecord::Missing) return TintaJournalResult::Unavailable;
    if (status == BookmarkPublicationRecord::Error) return TintaJournalResult::IoError;
    if (status != BookmarkPublicationRecord::Matches) return TintaJournalResult::Corrupt;
    if (loaded.transaction != publication.transaction || loaded.edition != publication.edition ||
        loaded.storageGeneration != publication.storageGeneration)
      return TintaJournalResult::Conflict;
    if (!validateContext(loaded)) return TintaJournalResult::Invalid;
    status = ownership(loaded);
    if (status == BookmarkPublicationRecord::Error) return TintaJournalResult::IoError;
    return status == BookmarkPublicationRecord::Matches ? TintaJournalResult::Ok : TintaJournalResult::Corrupt;
  }
  BookmarkPublicationRecord publication() override {
    const auto active = lookup.inspect(HalBookmarkPublicationPaths::INTENT);
    if (active == CompanionFilePresence::Error) return BookmarkPublicationRecord::Error;
    if (active == CompanionFilePresence::Present) return BookmarkPublicationRecord::Other;
    const auto pending = lookup.inspect(HalBookmarkPublicationPaths::INTENT_NEXT);
    if (pending == CompanionFilePresence::Error) return BookmarkPublicationRecord::Error;
    return pending == CompanionFilePresence::Missing ? BookmarkPublicationRecord::Missing
                                                     : BookmarkPublicationRecord::Other;
  }
  BookmarkPreparationFile file(BookmarkPreparationRole role) override {
    const char* path = spool(role);
    if (!path) return BookmarkPreparationFile::Error;
    const auto presence = lookup.inspect(path);
    if (presence == CompanionFilePresence::Missing) return BookmarkPreparationFile::Missing;
    if (presence == CompanionFilePresence::Error) return BookmarkPreparationFile::Error;
    if (!Storage.openFileForReadReusing("COMPANION", path, handle)) return BookmarkPreparationFile::Error;
    const bool directory = handle.isDirectory();
    if (!handle.close()) return BookmarkPreparationFile::Error;
    return directory ? BookmarkPreparationFile::Other : BookmarkPreparationFile::Regular;
  }
  bool persist(const BookmarkPreparationClaim& claim) override {
    return validateContext(claim) && publication() == BookmarkPublicationRecord::Missing &&
           records.persist(PATH, NEXT, claim);
  }
  bool remove(BookmarkPreparationRole role) override {
    if (!authorized()) return failure("remove authority");
    const auto present = file(role);
    if (present == BookmarkPreparationFile::Missing) return true;
    if (present != BookmarkPreparationFile::Regular || !authorized() || !Storage.remove(spool(role)))
      return failure("remove spool");
    return file(role) == BookmarkPreparationFile::Missing ? true : failure("remove readback");
  }
  bool clear(const BookmarkPreparationClaim& claim) override {
    if (!validateContext(claim) || !authorized()) return failure("clear authority");
    for (const auto role :
         {BookmarkPreparationRole::Identities, BookmarkPreparationRole::Bodies, BookmarkPreparationRole::Json})
      if (file(role) != BookmarkPreparationFile::Missing) return failure("clear live spool");
    return authorized() && records.remove(NEXT, claim) && authorized() && records.remove(PATH, claim);
  }

 private:
  bool authorized() {
    return bound && validateContext(binding) && ownership(binding) == BookmarkPublicationRecord::Matches &&
           publication() == BookmarkPublicationRecord::Missing;
  }
  static const char* spool(BookmarkPreparationRole role) {
    switch (role) {
      case BookmarkPreparationRole::Identities:
        return HalBookmarkIdentityStage::PATH;
      case BookmarkPreparationRole::Bodies:
        return HalBookmarkBodyStage::PATH;
      case BookmarkPreparationRole::Json:
        return HalBookmarkJsonStage::PATH;
    }
    return nullptr;
  }
  bool failure(const char* operation) {
    LOG_ERR("COMPANION", "Bookmark preparation storage %s failed", operation);
    return false;
  }
  HalBookmarkPreparationRecord records;
  HalCompanionFileLookup lookup;
  HalFile handle;
  BookmarkPreparationClaim binding{}, loaded{};
  Validator validator;
  void* context;
  bool bound = false;
};
}  // namespace companion
