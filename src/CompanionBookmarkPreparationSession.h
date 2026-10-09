#pragma once

#include <Memory.h>

#include "HalBookmarkPathHash.h"
#include "HalBookmarkPreparationStorage.h"

namespace companion {
// Checked off-stack owner; caller freezes verified book/card and excludes writers.
// Destruction retains durable authority for recovery rather than deleting spools.
class NativeBookmarkPreparationSession final {
 public:
  using Validator = HalBookmarkPreparationStorage::Validator;
  NativeBookmarkPreparationSession(Validator validator, void* context) : validator(validator), context(context) {
    mbedtls_sha256_init(&sha);
  }
  ~NativeBookmarkPreparationSession() { mbedtls_sha256_free(&sha); }
  NativeBookmarkPreparationSession(const NativeBookmarkPreparationSession&) = delete;
  NativeBookmarkPreparationSession& operator=(const NativeBookmarkPreparationSession&) = delete;
  TintaJournalResult begin(std::string_view active, const Digest& edition, const Identity& transaction,
                           const Identity& generation) {
    if (!tinta_body_detail::nonzero(transaction)) return TintaJournalResult::Invalid;
    const auto initialized = initialize(active, edition, generation);
    if (initialized != TintaJournalResult::Ok) return initialized;
    claim.transaction = transaction;
    expectedTransaction = transaction;
    BookmarkPreparation preparation(*storage);
    const auto result = preparation.begin(claim);
    ready = result == TintaJournalResult::Ok;
    return result;
  }
  TintaJournalResult recover(std::string_view active, const Digest& edition, const Identity& generation,
                             const Identity* transaction = nullptr) {
    if (transaction && !tinta_body_detail::nonzero(*transaction)) return TintaJournalResult::Invalid;
    if (transaction) expectedTransaction = *transaction;
    const auto initialized = initialize(active, edition, generation);
    if (initialized != TintaJournalResult::Ok) return initialized;
    const auto loaded = storage->load(claim);
    if (loaded == BookmarkPublicationRecord::Missing) return TintaJournalResult::Unavailable;
    if (loaded == BookmarkPublicationRecord::Error) return TintaJournalResult::IoError;
    if (loaded != BookmarkPublicationRecord::Matches) return TintaJournalResult::Corrupt;
    BookmarkPreparation preparation(*storage);
    return preparation.recover(claim);
  }
  TintaJournalResult guard() {
    if (!ready || !storage) return TintaJournalResult::Invalid;
    BookmarkPreparation preparation(*storage);
    return preparation.guard(claim);
  }
  TintaJournalResult validatePublication(std::string_view active, const BookmarkPublicationClaim& publication) {
    const auto initialized = initialize(active, publication.edition, publication.storageGeneration);
    if (initialized != TintaJournalResult::Ok) return initialized;
    expectedTransaction = publication.transaction;
    return storage->validatePublication(publication);
  }
  TintaJournalResult discard() {
    if (!ready || !storage) return TintaJournalResult::Invalid;
    BookmarkPreparation preparation(*storage);
    const auto result = preparation.recover(claim);
    if (result == TintaJournalResult::Ok) ready = false;
    return result;
  }
  const BookmarkPreparationClaim& preparationClaim() const { return claim; }

 private:
  TintaJournalResult initialize(std::string_view active, const Digest& edition, const Identity& generation) {
    if (used || !validator || !tinta_body_detail::nonzero(edition) || !tinta_body_detail::nonzero(generation) ||
        !hal_filename::valid(active) || !bookmarkCacheParent(active, edition))
      return TintaJournalResult::Invalid;
    used = true;
    expectedEdition = edition;
    expectedGeneration = generation;
    static constexpr uint8_t DOMAIN[] = "lila-bookmark-preparation-path-v1";
    if (mbedtls_sha256_starts(&sha, 0) || mbedtls_sha256_update(&sha, DOMAIN, sizeof(DOMAIN) - 1))
      return failure("path hash start");
    if (!updateBookmarkPathHash(sha, active)) return failure("path hash update");
    if (mbedtls_sha256_finish(&sha, expectedPath.data())) return failure("path hash finish");
    claim.edition = expectedEdition;
    claim.storageGeneration = expectedGeneration;
    claim.destinationPathHash = expectedPath;
    // Lookup buffers/handles exceed the task-local limit; allocate once per batch.
    storage = makeUniqueNoThrow<HalBookmarkPreparationStorage>(&validate, this);
    return storage ? TintaJournalResult::Ok : failure("storage allocation");
  }
  static bool validate(void* opaque, const BookmarkPreparationClaim& candidate) {
    auto& self = *static_cast<NativeBookmarkPreparationSession*>(opaque);
    return (!tinta_body_detail::nonzero(self.expectedTransaction) ||
            candidate.transaction == self.expectedTransaction) &&
           candidate.edition == self.expectedEdition && candidate.storageGeneration == self.expectedGeneration &&
           candidate.destinationPathHash == self.expectedPath && self.validator(self.context, candidate);
  }
  static TintaJournalResult failure(const char* operation) {
    LOG_ERR("COMPANION", "Bookmark preparation session %s failed", operation);
    return TintaJournalResult::IoError;
  }
  std::unique_ptr<HalBookmarkPreparationStorage> storage;
  BookmarkPreparationClaim claim{};
  Digest expectedEdition{}, expectedPath{};
  Identity expectedGeneration{}, expectedTransaction{};
  mbedtls_sha256_context sha;
  Validator validator;
  void* context;
  bool used = false, ready = false;
};
}  // namespace companion
