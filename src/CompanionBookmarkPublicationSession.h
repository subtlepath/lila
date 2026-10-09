#pragma once

#include "CompanionBookmarkJsonPreparation.h"
#include "CompanionBookmarkPreparationSession.h"
#include "HalBookmarkPublicationPaths.h"

namespace companion {
// Checked off-stack owner. Caller has imported legacy bookmarks, verified edition
// and card generation, and holds exclusive journal/bookmark write authority.
class NativeBookmarkPublicationSession final {
 public:
  using ContextValidator = HalBookmarkPublicationStorage::Validator;
  using PreparationValidator = NativeBookmarkPreparationSession::Validator;
  NativeBookmarkPublicationSession(std::span<uint8_t> scratch, ContextValidator validator, void* context,
                                   PreparationValidator preparationValidator)
      : preparation(scratch, &guardPreparation, this),
        scratch(scratch),
        validator(validator),
        context(context),
        preparationValidator(preparationValidator) {}
  TintaJournalResult publish(std::string_view activePath, const Digest& edition, uint32_t spineCount,
                             const Identity& transaction, const Identity& storageGeneration, bool allowEmpty = false) {
    if (used || !validator || !preparationValidator || !spineCount || spineCount > 65536 ||
        !tinta_body_detail::nonzero(transaction) || !tinta_body_detail::nonzero(storageGeneration))
      return TintaJournalResult::Invalid;
    used = true;
    claim.transaction = transaction;
    claim.storageGeneration = storageGeneration;
    claim.edition = edition;
    // Retain one checked ownership owner through preparation and intent handoff.
    ownership = makeUniqueNoThrow<NativeBookmarkPreparationSession>(preparationValidator, context);
    if (!ownership) {
      LOG_ERR("COMPANION", "OOM: bookmark preparation authority");
      return TintaJournalResult::IoError;
    }
    const auto acquired = ownership->begin(activePath, edition, transaction, storageGeneration);
    if (acquired != TintaJournalResult::Ok) {
      recoveryRequired = acquired == TintaJournalResult::IoError;
      return acquired;
    }
    recoveryRequired = true;
    const auto prepared = preparation.prepare(edition, spineCount, allowEmpty);
    if (prepared != TintaJournalResult::Ok) return discard(prepared);
    claim.edition = preparation.contentEdition();
    claim.frontier = preparation.authorityFrontier();
    claim.candidateHash = preparation.contentHash();
    claim.candidateLength = preparation.bytesWritten();
    claim.recordCount = preparation.recordCount();
    claim.recordSize = preparation.recordSize();
    if (!attach(activePath)) return discard(attachFailure);
    if (!storage->validateContext(claim)) return discard(TintaJournalResult::Invalid);
    if (!storage->describeOriginal(claim)) return discard(TintaJournalResult::IoError);
    if (!storage->validateAuthority(claim)) return discard(authorityFailure);
    if (claim.hadOriginal && claim.originalHash == claim.candidateHash && claim.originalLength == claim.candidateLength)
      return discard(TintaJournalResult::Ok);
    const auto pending = storage->intent(claim);
    const auto receipt = storage->receipt(claim);
    const auto candidate = storage->file(BookmarkPublicationRole::Candidate, claim);
    const auto backup = storage->file(BookmarkPublicationRole::Backup, claim);
    if (pending == BookmarkPublicationRecord::Error || receipt == BookmarkPublicationRecord::Error ||
        candidate == BookmarkPublicationFile::Error || backup == BookmarkPublicationFile::Error)
      return discard(TintaJournalResult::IoError);
    if (pending != BookmarkPublicationRecord::Missing || receipt != BookmarkPublicationRecord::Missing ||
        candidate != BookmarkPublicationFile::Candidate || backup != BookmarkPublicationFile::Missing)
      return discard(TintaJournalResult::Corrupt);
    if (!storage->validateContext(claim)) return discard(TintaJournalResult::Invalid);
    recoveryRequired = true;
    if (!preparation.persistPublicationIntent(*storage, claim)) return TintaJournalResult::IoError;
    BookmarkPublication publication(*storage);
    const auto result = publication.recover(claim);
    return result == TintaJournalResult::Ok ? retirePreparation(activePath) : result;
  }
  TintaJournalResult recover(std::string_view activePath, const BookmarkPublicationClaim& pending) {
    if (used || !validBookmarkPublicationClaim(pending)) return TintaJournalResult::Invalid;
    used = true;
    claim = pending;
    recoveryRequired = true;
    if (!attach(activePath)) return attachFailure;
    const auto intent = storage->intent(claim);
    if (intent == BookmarkPublicationRecord::Error) return TintaJournalResult::IoError;
    if (intent == BookmarkPublicationRecord::Other) return TintaJournalResult::Corrupt;
    if (intent == BookmarkPublicationRecord::Matches) {
      const auto bound = validatePreparation(activePath);
      if (bound != TintaJournalResult::Ok) return bound;
    }
    BookmarkPublication publication(*storage);
    const auto result = publication.recover(claim);
    return result == TintaJournalResult::Ok ? retirePreparation(activePath) : result;
  }
  // Reload ownership after reboot; a verified temporary claim is synced and
  // promoted only after the caller validates the current card/content context.
  TintaJournalResult recoverPending(std::string_view activePath) {
    if (used || !validator) return TintaJournalResult::Invalid;
    used = true;
    auto records = makeUniqueNoThrow<HalBookmarkPublicationRecord>();
    if (!records) {
      LOG_ERR("COMPANION", "OOM: bookmark recovery record");
      return TintaJournalResult::IoError;
    }
    auto pending = records->read(HalBookmarkPublicationPaths::INTENT, claim);
    const bool temporary = pending == BookmarkPublicationRecord::Missing;
    if (temporary) pending = records->read(HalBookmarkPublicationPaths::INTENT_NEXT, claim);
    records.reset();
    if (pending == BookmarkPublicationRecord::Missing) return TintaJournalResult::Unavailable;
    if (pending == BookmarkPublicationRecord::Error) return TintaJournalResult::IoError;
    if (pending != BookmarkPublicationRecord::Matches) return TintaJournalResult::Corrupt;
    recoveryRequired = true;
    if (!attach(activePath)) return attachFailure;
    if (!storage->validateContext(claim)) return TintaJournalResult::Invalid;
    const auto bound = validatePreparation(activePath);
    if (bound != TintaJournalResult::Ok) return bound;
    if (temporary && !storage->persistIntent(claim)) return TintaJournalResult::IoError;
    BookmarkPublication publication(*storage);
    const auto result = publication.recover(claim);
    return result == TintaJournalResult::Ok ? retirePreparation(activePath) : result;
  }
  bool requiresRecovery() const { return recoveryRequired; }
  const BookmarkPublicationClaim& publicationClaim() const { return claim; }
  const Identity& conflictIdentity() const { return preparation.conflictIdentity(); }

 private:
  TintaJournalResult validatePreparation(std::string_view activePath) {
    auto preparation = makeUniqueNoThrow<NativeBookmarkPreparationSession>(preparationValidator, context);
    if (!preparation) {
      LOG_ERR("COMPANION", "OOM: bookmark recovery path proof");
      return TintaJournalResult::IoError;
    }
    const auto result = preparation->validatePublication(activePath, claim);
    if (result == TintaJournalResult::Unavailable && claim.recordCount &&
        bookmarkCacheParent(activePath, claim.edition) == BOOKMARK_CACHE_PARENT)
      return TintaJournalResult::Ok;
    if (result != TintaJournalResult::Ok)
      LOG_ERR("COMPANION", "Bookmark recovery preparation refused: %u", static_cast<unsigned>(result));
    return result == TintaJournalResult::Unavailable ? TintaJournalResult::Corrupt : result;
  }
  bool attach(std::string_view activePath) {
    if (!paths.initialize(activePath, claim)) return false;
    // Separate allocation is made once, after temporary preparation audits finish.
    storage =
        makeUniqueNoThrow<HalBookmarkPublicationStorage>(paths.paths(), scratch, &checkContext, &checkAuthority, this);
    if (!storage) {
      attachFailure = TintaJournalResult::IoError;
      LOG_ERR("COMPANION", "OOM: bookmark publication storage");
      return false;
    }
    return true;
  }
  static bool checkContext(void* opaque, const BookmarkPublicationClaim& claim) {
    auto& self = *static_cast<NativeBookmarkPublicationSession*>(opaque);
    return self.paths.matches(claim) && self.validator && self.validator(self.context, claim);
  }
  static bool checkAuthority(void* opaque, const BookmarkPublicationClaim& claim) {
    auto& self = *static_cast<NativeBookmarkPublicationSession*>(opaque);
    self.authorityFailure = TintaJournalResult::IoError;
    auto audit = makeUniqueNoThrow<HalJournalCausalAuditSession>();
    if (!audit) {
      LOG_ERR("COMPANION", "OOM: bookmark publication audit");
      return false;
    }
    if (!audit->run(&self.rechecked)) return false;
    self.authorityFailure = TintaJournalResult::Conflict;
    return self.rechecked == claim.frontier && audit->recordCount() == claim.recordCount &&
           audit->recordSize() == claim.recordSize;
  }
  static bool guardPreparation(void* opaque) {
    auto& self = *static_cast<NativeBookmarkPublicationSession*>(opaque);
    return self.ownership && self.ownership->guard() == TintaJournalResult::Ok;
  }
  TintaJournalResult retirePreparation(std::string_view activePath) {
    TintaJournalResult retired;
    if (ownership) {
      retired = ownership->discard();
    } else {
      auto recovery = makeUniqueNoThrow<NativeBookmarkPreparationSession>(preparationValidator, context);
      if (!recovery) {
        LOG_ERR("COMPANION", "OOM: bookmark preparation retirement");
        return TintaJournalResult::IoError;
      }
      retired = recovery->recover(activePath, claim.edition, claim.storageGeneration, &claim.transaction);
    }
    if (retired != TintaJournalResult::Ok && retired != TintaJournalResult::Unavailable) return retired;
    recoveryRequired = false;
    return TintaJournalResult::Ok;
  }
  TintaJournalResult discard(TintaJournalResult result) {
    if (!preparation.cleanup() || !ownership || ownership->discard() != TintaJournalResult::Ok)
      return TintaJournalResult::IoError;
    recoveryRequired = false;
    return result;
  }
  // Ownership outlives stage destructors and their borrowed guard callback.
  std::unique_ptr<NativeBookmarkPreparationSession> ownership;
  NativeBookmarkJsonPreparation preparation;
  HalBookmarkPublicationPaths paths;
  std::span<uint8_t> scratch;
  ContextValidator validator;
  void* context;
  PreparationValidator preparationValidator;
  BookmarkPublicationClaim claim{};
  Digest rechecked{};
  std::unique_ptr<HalBookmarkPublicationStorage> storage;
  TintaJournalResult attachFailure = TintaJournalResult::Invalid, authorityFailure = TintaJournalResult::Conflict;
  bool used = false, recoveryRequired = false;
};
}  // namespace companion
