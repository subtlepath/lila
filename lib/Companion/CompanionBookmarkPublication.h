#pragma once

#include "CompanionTintaJournal.h"

namespace companion {
struct BookmarkPublicationClaim {
  Identity transaction{}, storageGeneration{};
  Digest edition{}, frontier{}, candidateHash{}, originalHash{};
  uint64_t candidateLength = 0, originalLength = 0;
  uint32_t recordCount = 0;
  uint16_t recordSize = 0;
  bool hadOriginal = false;
  bool operator==(const BookmarkPublicationClaim&) const = default;
};
inline bool validBookmarkPublicationClaim(const BookmarkPublicationClaim& claim) {
  return tinta_body_detail::nonzero(claim.transaction) && tinta_body_detail::nonzero(claim.storageGeneration) &&
         tinta_body_detail::nonzero(claim.edition) && tinta_body_detail::nonzero(claim.frontier) &&
         tinta_body_detail::nonzero(claim.candidateHash) && claim.candidateLength &&
         (claim.recordSize == 512 || claim.recordSize == 1024) && claim.recordCount <= UINT32_MAX / claim.recordSize &&
         (claim.hadOriginal ? tinta_body_detail::nonzero(claim.originalHash)
                            : !claim.originalLength && !tinta_body_detail::nonzero(claim.originalHash));
}
enum class BookmarkPublicationRole : uint8_t { Active, Candidate, Backup };
enum class BookmarkPublicationRecord : uint8_t { Missing, Matches, Other, Error };
enum class BookmarkPublicationFile : uint8_t { Missing, Candidate, Original, Other, Error };
class BookmarkPublicationStorage {
 public:
  virtual ~BookmarkPublicationStorage() = default;
  virtual bool validateContext(const BookmarkPublicationClaim&) = 0;
  virtual bool validateAuthority(const BookmarkPublicationClaim&) = 0;
  virtual BookmarkPublicationRecord intent(const BookmarkPublicationClaim&) = 0;
  virtual BookmarkPublicationRecord receipt(const BookmarkPublicationClaim&) = 0;
  // Candidate/Original require verified exact length/hash and unambiguous regular files.
  virtual BookmarkPublicationFile file(BookmarkPublicationRole, const BookmarkPublicationClaim&) = 0;
  // Durable mutations preserve ownership; false may mean a lost acknowledgement.
  virtual bool persistIntent(const BookmarkPublicationClaim&) = 0;
  virtual bool move(BookmarkPublicationRole from, BookmarkPublicationRole to) = 0;
  virtual bool remove(BookmarkPublicationRole) = 0;
  virtual bool commitReceipt(const BookmarkPublicationClaim&) = 0;
  virtual bool clearIntent() = 0;
};
// Serialized owner. A verified durable intent authorizes forward recovery even
// if later journal changes require another replay before the reader resumes.
class BookmarkPublication final {
 public:
  explicit BookmarkPublication(BookmarkPublicationStorage& storage) : storage(storage) {}
  TintaJournalResult publish(const BookmarkPublicationClaim& claim) {
    if (!validBookmarkPublicationClaim(claim) || !storage.validateContext(claim)) return TintaJournalResult::Invalid;
    const auto pending = storage.intent(claim);
    if (pending == BookmarkPublicationRecord::Error) return TintaJournalResult::IoError;
    if (pending == BookmarkPublicationRecord::Matches) return recover(claim);
    if (pending != BookmarkPublicationRecord::Missing) return TintaJournalResult::Corrupt;
    if (!storage.validateAuthority(claim)) return TintaJournalResult::Conflict;
    const auto committed = storage.receipt(claim);
    if (committed == BookmarkPublicationRecord::Error) return TintaJournalResult::IoError;
    if (committed == BookmarkPublicationRecord::Matches) return finish(claim, false);
    const auto active = storage.file(BookmarkPublicationRole::Active, claim);
    const auto candidate = storage.file(BookmarkPublicationRole::Candidate, claim);
    const auto backup = storage.file(BookmarkPublicationRole::Backup, claim);
    if (hasError(active, candidate, backup)) return TintaJournalResult::IoError;
    if (candidate != BookmarkPublicationFile::Candidate || backup != BookmarkPublicationFile::Missing ||
        (claim.hadOriginal ? !isOriginal(active, claim) : active != BookmarkPublicationFile::Missing))
      return TintaJournalResult::Corrupt;
    if (!storage.persistIntent(claim)) return TintaJournalResult::IoError;
    return recover(claim);
  }
  TintaJournalResult recover(const BookmarkPublicationClaim& claim) {
    if (!validBookmarkPublicationClaim(claim) || !storage.validateContext(claim)) return TintaJournalResult::Invalid;
    const auto pending = storage.intent(claim);
    const auto committed = storage.receipt(claim);
    if (pending == BookmarkPublicationRecord::Error || committed == BookmarkPublicationRecord::Error)
      return TintaJournalResult::IoError;
    if (pending == BookmarkPublicationRecord::Missing)
      return committed == BookmarkPublicationRecord::Matches ? finish(claim, false) : TintaJournalResult::Unavailable;
    if (pending != BookmarkPublicationRecord::Matches) return TintaJournalResult::Corrupt;
    if (committed == BookmarkPublicationRecord::Matches) return finish(claim, true);
    const auto active = storage.file(BookmarkPublicationRole::Active, claim);
    const auto candidate = storage.file(BookmarkPublicationRole::Candidate, claim);
    const auto backup = storage.file(BookmarkPublicationRole::Backup, claim);
    if (hasError(active, candidate, backup)) return TintaJournalResult::IoError;
    if ((candidate != BookmarkPublicationFile::Missing && candidate != BookmarkPublicationFile::Candidate) ||
        (backup != BookmarkPublicationFile::Missing && (!claim.hadOriginal || !isOriginal(backup, claim))))
      return TintaJournalResult::Corrupt;
    if (active != BookmarkPublicationFile::Candidate) {
      if (candidate != BookmarkPublicationFile::Candidate) return TintaJournalResult::Corrupt;
      if (claim.hadOriginal && isOriginal(active, claim)) {
        if (backup != BookmarkPublicationFile::Missing) return TintaJournalResult::Corrupt;
        if (!storage.validateContext(claim)) return TintaJournalResult::Invalid;
        if (!storage.move(BookmarkPublicationRole::Active, BookmarkPublicationRole::Backup))
          return TintaJournalResult::IoError;
      } else if (active != BookmarkPublicationFile::Missing ||
                 (claim.hadOriginal ? !isOriginal(backup, claim) : backup != BookmarkPublicationFile::Missing)) {
        return TintaJournalResult::Corrupt;
      }
      if (!storage.validateContext(claim)) return TintaJournalResult::Invalid;
      if (!storage.move(BookmarkPublicationRole::Candidate, BookmarkPublicationRole::Active))
        return TintaJournalResult::IoError;
    } else if (claim.hadOriginal && !sameBytes(claim) && !isOriginal(backup, claim)) {
      return TintaJournalResult::Corrupt;
    }
    const auto installed = storage.file(BookmarkPublicationRole::Active, claim);
    if (installed == BookmarkPublicationFile::Error) return TintaJournalResult::IoError;
    if (installed != BookmarkPublicationFile::Candidate) return TintaJournalResult::Corrupt;
    if (!storage.validateContext(claim)) return TintaJournalResult::Invalid;
    if (!storage.commitReceipt(claim)) return TintaJournalResult::IoError;
    return finish(claim, true);
  }

 private:
  static bool sameBytes(const BookmarkPublicationClaim& claim) {
    return claim.hadOriginal && claim.originalLength == claim.candidateLength &&
           claim.originalHash == claim.candidateHash;
  }
  static bool isOriginal(BookmarkPublicationFile file, const BookmarkPublicationClaim& claim) {
    return file == BookmarkPublicationFile::Original ||
           (file == BookmarkPublicationFile::Candidate && sameBytes(claim));
  }
  static bool hasError(BookmarkPublicationFile a, BookmarkPublicationFile b, BookmarkPublicationFile c) {
    return a == BookmarkPublicationFile::Error || b == BookmarkPublicationFile::Error ||
           c == BookmarkPublicationFile::Error;
  }
  TintaJournalResult finish(const BookmarkPublicationClaim& claim, bool clearPending) {
    const auto committed = storage.receipt(claim);
    if (committed == BookmarkPublicationRecord::Error) return TintaJournalResult::IoError;
    if (committed != BookmarkPublicationRecord::Matches) return TintaJournalResult::Corrupt;
    const auto active = storage.file(BookmarkPublicationRole::Active, claim);
    const auto candidate = storage.file(BookmarkPublicationRole::Candidate, claim);
    const auto backup = storage.file(BookmarkPublicationRole::Backup, claim);
    if (hasError(active, candidate, backup)) return TintaJournalResult::IoError;
    if (active != BookmarkPublicationFile::Candidate ||
        (candidate != BookmarkPublicationFile::Missing && candidate != BookmarkPublicationFile::Candidate) ||
        (backup != BookmarkPublicationFile::Missing && (!claim.hadOriginal || !isOriginal(backup, claim))))
      return TintaJournalResult::Corrupt;
    if (candidate == BookmarkPublicationFile::Candidate) {
      if (!storage.validateContext(claim)) return TintaJournalResult::Invalid;
      if (!storage.remove(BookmarkPublicationRole::Candidate)) return TintaJournalResult::IoError;
    }
    if (backup != BookmarkPublicationFile::Missing) {
      if (!storage.validateContext(claim)) return TintaJournalResult::Invalid;
      if (!storage.remove(BookmarkPublicationRole::Backup)) return TintaJournalResult::IoError;
    }
    if (storage.file(BookmarkPublicationRole::Candidate, claim) != BookmarkPublicationFile::Missing ||
        storage.file(BookmarkPublicationRole::Backup, claim) != BookmarkPublicationFile::Missing)
      return TintaJournalResult::IoError;
    if (clearPending) {
      if (!storage.validateContext(claim)) return TintaJournalResult::Invalid;
      if (!storage.clearIntent()) return TintaJournalResult::IoError;
    }
    return clearPending && storage.intent(claim) != BookmarkPublicationRecord::Missing ? TintaJournalResult::IoError
                                                                                       : TintaJournalResult::Ok;
  }
  BookmarkPublicationStorage& storage;
};
}  // namespace companion
