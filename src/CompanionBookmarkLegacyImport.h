#pragma once

#include <mbedtls/sha256.h>

#include "BookmarkEntry.h"
#include "CompanionBookmarkSaveSession.h"

namespace companion {
// Checked off-stack owner. The verified original file hash must come from the
// retained legacy backup. Input entries stay frozen throughout import.
class NativeBookmarkLegacyImport final {
 public:
  NativeBookmarkLegacyImport() { mbedtls_sha256_init(&sha); }
  ~NativeBookmarkLegacyImport() { mbedtls_sha256_free(&sha); }
  NativeBookmarkLegacyImport(const NativeBookmarkLegacyImport&) = delete;
  NativeBookmarkLegacyImport& operator=(const NativeBookmarkLegacyImport&) = delete;
  TintaJournalResult associateMissingIdentities(std::span<BookmarkEntry> entries, const Digest& edition) {
    if (used || associated || !tinta_body_detail::nonzero(edition)) return TintaJournalResult::Invalid;
    associated = true;
    if (entries.empty()) return TintaJournalResult::Ok;
    auto audit = makeUniqueNoThrow<HalJournalCausalAuditSession>();
    if (!audit) return failure("association audit allocation");
    for (auto& entry : entries) {
      if (!BookmarkIdentity::valid(entry.identity)) continue;
      if (!audit->run()) return failure("association authority audit");
      size_t length = 0;
      const auto result = audit->resolveBookmark(edition, entry.identity, body, length);
      if (result == TintaJournalResult::Unavailable) {
        entry.identity = {};
      } else if (result != TintaJournalResult::Ok) {
        if (result == TintaJournalResult::Conflict) conflict = entry.identity;
        return result;
      }
      vTaskDelay(1);
    }
    return TintaJournalResult::Ok;
  }
  bool identityFor(const BookmarkEntry& entry, uint64_t ordinal, const Digest& edition, const Digest& originalHash,
                   Identity& output) {
    if (!tinta_body_detail::nonzero(edition) || !tinta_body_detail::nonzero(originalHash)) return false;
    if (BookmarkIdentity::valid(entry.identity)) {
      output = entry.identity;
      return true;
    }
    static constexpr uint8_t DOMAIN[] = {'l', 'i', 'l', 'a', '-', 'b', 'o', 'o', 'k', 'm', 'a',
                                         'r', 'k', '-', 'l', 'e', 'g', 'a', 'c', 'y', 1};
    std::array<uint8_t, 8> index{};
    for (auto& byte : index) {
      byte = static_cast<uint8_t>(ordinal);
      ordinal >>= 8;
    }
    if (mbedtls_sha256_starts(&sha, 0) || mbedtls_sha256_update(&sha, DOMAIN, sizeof(DOMAIN)) ||
        mbedtls_sha256_update(&sha, edition.data(), edition.size()) ||
        mbedtls_sha256_update(&sha, originalHash.data(), originalHash.size()) ||
        mbedtls_sha256_update(&sha, index.data(), index.size()) || mbedtls_sha256_finish(&sha, derived.data())) {
      LOG_ERR("COMPANION", "Legacy bookmark identity SHA failed");
      return false;
    }
    if (!tinta_body_detail::nonzero(std::span(derived).first(16))) return false;
    std::copy_n(derived.begin(), output.size(), output.begin());
    return true;
  }
  // Initial import fills missing authority only; existing puts/deletions are
  // preserved. Legacy positions without exact offsets must be resolved first.
  TintaJournalResult import(std::span<const BookmarkEntry> entries, const Digest& edition, const Digest& originalHash,
                            uint32_t spineCount, IdentityStorage& identities) {
    if (used) return TintaJournalResult::Unavailable;
    used = true;
    if (!spineCount || spineCount > 65536 || !tinta_body_detail::nonzero(edition) ||
        !tinta_body_detail::nonzero(originalHash))
      return TintaJournalResult::Invalid;
    // Validate every entry and conflict before the first journal mutation.
    for (size_t i = 0; i < entries.size(); ++i) {
      if (!view(entries[i], i, edition, originalHash, spineCount)) return TintaJournalResult::Invalid;
      for (size_t j = 0; j < i; ++j) {
        if (!identityFor(entries[j], j, edition, originalHash, previous)) return failure("identity preflight");
        if (previous == bookmark.identity) return TintaJournalResult::Invalid;
        if ((j & 31U) == 31U) vTaskDelay(1);
      }
      vTaskDelay(1);
    }
    if (entries.empty()) return TintaJournalResult::Ok;
    auto audit = makeUniqueNoThrow<HalJournalCausalAuditSession>();
    if (!audit) return failure("audit allocation");
    for (size_t i = 0; i < entries.size(); ++i) {
      if (!view(entries[i], i, edition, originalHash, spineCount)) return failure("preflight refresh");
      const auto resolved = resolve(*audit, edition, bookmark.identity, BookmarkIdentity::valid(entries[i].identity));
      if (resolved != TintaJournalResult::Ok && resolved != TintaJournalResult::Unavailable) return resolved;
      vTaskDelay(1);
    }
    for (size_t i = 0; i < entries.size(); ++i) {
      if (!view(entries[i], i, edition, originalHash, spineCount)) return finish(failure("entry refresh"));
      const auto resolved = resolve(*audit, edition, bookmark.identity, BookmarkIdentity::valid(entries[i].identity));
      if (resolved == TintaJournalResult::Ok) continue;
      if (resolved != TintaJournalResult::Unavailable) return finish(resolved);
      const auto saved = append(*audit, edition, identities);
      if (saved != TintaJournalResult::Ok) return finish(saved);
      vTaskDelay(1);
    }
    return finish(TintaJournalResult::Ok);
  }
  bool requiresRecovery() const { return recovery; }
  const Identity& conflictIdentity() const { return conflict; }

 private:
  bool view(const BookmarkEntry& entry, uint64_t ordinal, const Digest& edition, const Digest& originalHash,
            uint32_t spineCount) {
    if (!entry.hasVisibleTextOffset || entry.computedSpineIndex >= spineCount ||
        !identityFor(entry, ordinal, edition, originalHash, bookmark.identity))
      return false;
    bookmark.deleted = false;
    bookmark.anchor = {entry.computedSpineIndex, entry.visibleTextOffset};
    bookmark.name = {reinterpret_cast<const uint8_t*>(entry.name.data()), entry.name.size()};
    bookmark.summary = {reinterpret_cast<const uint8_t*>(entry.summary.data()), entry.summary.size()};
    return encodeBookmarkBody(bookmark, body) != 0;
  }
  TintaJournalResult resolve(HalJournalCausalAuditSession& audit, const Digest& edition, const Identity& identity,
                             bool explicitIdentity) {
    if (!audit.run()) return failure("authority audit");
    size_t length = 0;
    const auto result = audit.resolveBookmark(edition, identity, body, length);
    if (result == TintaJournalResult::Conflict) conflict = identity;
    if (result == TintaJournalResult::Unavailable && explicitIdentity) {
      if (!audit.run()) return failure("edition proof audit");
      const auto checked = audit.checkBookmarkEdition(edition, identity);
      if (checked != TintaJournalResult::Ok) return checked;
    }
    return result;
  }
  TintaJournalResult append(HalJournalCausalAuditSession& audit, const Digest& edition, IdentityStorage& identities) {
    const auto length = encodeBookmarkBody(bookmark, body);
    if (!length || !audit.run()) return failure("append preflight");
    if (!started) {
      const auto result = writer.start(identities);
      if (result != TintaJournalResult::Ok) return failure("writer start");
      started = true;
    }
    auto* heads = audit.beginPreferenceKnowledge(writer.authority().count());
    if (!heads) return failure("knowledge snapshot");
    const auto result =
        writer.mutations().recordBookmark(std::span(body).first(length), edition, *heads, 0, ClockQuality::Unknown);
    recovery = recovery || !writer.mutations().available();
    if (!audit.endPreferenceKnowledge()) return failure("knowledge close");
    return result;
  }
  TintaJournalResult finish(TintaJournalResult result) { return writer.close() ? result : failure("writer close"); }
  TintaJournalResult failure(const char* stage) {
    recovery = true;
    conflict = {};
    LOG_ERR("COMPANION", "Legacy bookmark import %s failed", stage);
    return TintaJournalResult::IoError;
  }
  HalTintaWriterSession writer;
  mbedtls_sha256_context sha;
  Digest derived{};
  Identity previous{}, conflict{};
  BookmarkBodyView bookmark;
  std::array<uint8_t, MAX_BOOKMARK_BODY_SIZE> body{};
  bool used = false, recovery = false, started = false, associated = false;
};
}  // namespace companion
