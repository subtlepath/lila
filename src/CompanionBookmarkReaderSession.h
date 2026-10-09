#pragma once

#include <optional>

#include "CompanionBookmarkEditSession.h"
#include "CompanionBookmarkMigrationSession.h"
#include "CompanionBookmarkPublicationSession.h"
#include "CompanionBookmarkReaderBinding.h"

namespace companion {
// Checked off-stack one-shot lifecycle owner. Caller freezes book/card context
// and serializes journal/cache writers; scratch and callback context outlive it.
// Enable edits only after successful restore for this book/card context.
class NativeBookmarkReaderSession final {
 public:
  using Gate = bool (*)(void*);
  using Release = void (*)(void*);
  using Matcher = bool (*)(void*, const BookmarkEntry&);
  NativeBookmarkReaderSession(std::span<uint8_t> scratch, const Digest& edition, const Identity& generation,
                              IdentityStorage& identities, Gate gate,
                              NativeBookmarkMigrationSession::Validator migrationValidator,
                              NativeBookmarkPublicationSession::ContextValidator publicationValidator,
                              NativeBookmarkPublicationSession::PreparationValidator preparationValidator,
                              void* context, Release release = nullptr)
      : scratch(scratch),
        edition(edition),
        generation(generation),
        identities(identities),
        gate(gate),
        migrationValidator(migrationValidator),
        publicationValidator(publicationValidator),
        preparationValidator(preparationValidator),
        context(context),
        release(release) {}
  TintaJournalResult restore(const char* path, uint32_t spineCount, std::vector<BookmarkEntry>& entries,
                             NativeBookmarkMigrationSession::AnchorResolver anchors = nullptr) {
    // Reuse the vector for parsing, but expose rows only after full restore.
    struct Output {
      std::vector<BookmarkEntry>& entries;
      bool published = false;
      ~Output() {
        if (!published) entries.clear();
      }
    } output{entries};
    if (!initialize(path, spineCount) || editionCache) return TintaJournalResult::Invalid;
    auto result = recoverWorkspace();
    if (result != TintaJournalResult::Ok) return result;
    if (!gate(context) || !Storage.ensureDirectoryExists(HalBookmarkPublicationPaths::ACTIVE_PARENT))
      return failure("bookmark directory");
    const auto present = lookup->inspect(active.data());
    if (present == CompanionFilePresence::Error) return failure("source lookup");
    if (present == CompanionFilePresence::Present) {
      if (!randomTransaction()) return failure("migration transaction");
      auto migration = makeUniqueNoThrow<NativeBookmarkMigrationSession>(scratch, migrationValidator, context);
      if (!migration) return failure("migration allocation");
      result = migration->importLegacy(active.data(), edition, transaction, generation, spineCount, identities, entries,
                                       anchors);
      if (result != TintaJournalResult::Ok) {
        if (result == TintaJournalResult::Conflict && gate(context)) conflict = migration->conflictIdentity();
        return result;
      }
      migration.reset();
    }
    if (release) release(context);
    result = publishAndLoad(spineCount, entries);
    output.published = result == TintaJournalResult::Ok;
    return result;
  }
  TintaJournalResult restoreEdition(const char* path, const char* legacyPath, uint32_t spineCount,
                                    std::vector<BookmarkEntry>& entries,
                                    LegacyBookmarkDecision decision = LegacyBookmarkDecision::Undecided,
                                    NativeBookmarkMigrationSession::AnchorResolver anchors = nullptr) {
    struct Output {
      std::vector<BookmarkEntry>& entries;
      bool published = false;
      ~Output() {
        if (!published) entries.clear();
      }
    } output{entries};
    if (!initialize(path, spineCount) || !editionCache || !legacyPath ||
        (decision != LegacyBookmarkDecision::Undecided && decision != LegacyBookmarkDecision::Associate &&
         decision != LegacyBookmarkDecision::LeaveUnassociated))
      return TintaJournalResult::Invalid;
    const auto legacySize = strnlen(legacyPath, active.size());
    const std::string_view legacy(legacyPath, legacySize);
    if (legacySize == active.size() || !hal_filename::valid(legacy) ||
        bookmarkCacheParent(legacy, edition) != BOOKMARK_CACHE_PARENT)
      return TintaJournalResult::Invalid;
    auto result = recoverWorkspace(legacyPath);
    if (result != TintaJournalResult::Ok) return result;
    if (!gate(context) || !Storage.ensureDirectoryExists(BOOKMARK_EDITION_CACHE_PARENT))
      return failure("edition cache directory");
    const auto canonical = lookup->inspect(active.data());
    if (canonical == CompanionFilePresence::Error) return failure("edition cache lookup");
    if (canonical == CompanionFilePresence::Missing) {
      if (!Storage.ensureDirectoryExists(BOOKMARK_CACHE_PARENT)) return failure("legacy directory");
      lookup.emplace(nullptr, nullptr, BOOKMARK_CACHE_PARENT);
      const auto source = lookup->inspect(legacyPath);
      if (source == CompanionFilePresence::Error) return failure("legacy source lookup");
      if (!gate(context)) return failure("legacy decision context");
      if (source == CompanionFilePresence::Present) {
        if (decision == LegacyBookmarkDecision::Undecided) {
          associationRequired = true;
          recoveryRequired = false;
          return TintaJournalResult::Conflict;
        }
        if (decision == LegacyBookmarkDecision::Associate) {
          if (!randomTransaction()) return failure("association transaction");
          auto migration = makeUniqueNoThrow<NativeBookmarkMigrationSession>(scratch, migrationValidator, context);
          if (!migration) return failure("association allocation");
          result = migration->importLegacy(legacyPath, edition, transaction, generation, spineCount, identities,
                                           entries, anchors, true);
          if (result != TintaJournalResult::Ok) {
            if (result == TintaJournalResult::Conflict && gate(context)) conflict = migration->conflictIdentity();
            return result;
          }
        }
      }
      lookup.emplace(nullptr, nullptr, BOOKMARK_EDITION_CACHE_PARENT);
    }
    if (release) release(context);
    result = publishAndLoad(spineCount, entries);
    output.published = result == TintaJournalResult::Ok;
    return result;
  }
  TintaJournalResult prepareChoice(const char* path, uint32_t spineCount) {
    if (!initialize(path, spineCount)) return TintaJournalResult::Invalid;
    const auto recovered = recoverWorkspace();
    if (recovered != TintaJournalResult::Ok) return recovered;
    if (!gate(context)) return failure("choice context");
    recoveryRequired = false;
    return TintaJournalResult::Ok;
  }
  TintaJournalResult create(const char* path, uint32_t spineCount, BookmarkEntry& candidate,
                            std::vector<BookmarkEntry>& entries) {
    if (!initialize(path, spineCount)) return TintaJournalResult::Invalid;
    auto edit = makeUniqueNoThrow<NativeBookmarkEditSession>();
    if (!edit) return failure("edit allocation");
    const auto result = edit->create(candidate, edition, spineCount, identities);
    if (result != TintaJournalResult::Ok) {
      recoveryRequired = edit->requiresRecovery();
      return result;
    }
    edit.reset();
    return publishAndLoad(spineCount, entries);
  }
  TintaJournalResult rename(const char* path, uint32_t spineCount, const BookmarkEntry& selected, std::string_view name,
                            std::vector<BookmarkEntry>& entries) {
    if (!initialize(path, spineCount)) return TintaJournalResult::Invalid;
    auto edit = makeUniqueNoThrow<NativeBookmarkEditSession>();
    if (!edit) return failure("edit allocation");
    const auto result = edit->rename(selected, name, edition, spineCount, identities);
    if (result != TintaJournalResult::Ok) {
      recoveryRequired = edit->requiresRecovery();
      if (result == TintaJournalResult::Conflict && !recoveryRequired && gate(context)) conflict = selected.identity;
      return result;
    }
    edit.reset();
    return publishAndLoad(spineCount, entries);
  }
  TintaJournalResult erase(const char* path, uint32_t spineCount, const BookmarkEntry& selected,
                           std::vector<BookmarkEntry>& entries) {
    if (!initialize(path, spineCount)) return TintaJournalResult::Invalid;
    auto edit = makeUniqueNoThrow<NativeBookmarkEditSession>();
    if (!edit) return failure("edit allocation");
    const auto result = edit->erase(selected, edition, identities);
    if (result != TintaJournalResult::Ok) {
      recoveryRequired = edit->requiresRecovery();
      if (result == TintaJournalResult::Conflict && !recoveryRequired && gate(context)) conflict = selected.identity;
      return result;
    }
    edit.reset();
    return publishAndLoad(spineCount, entries);
  }
  bool requiresRecovery() const { return recoveryRequired; }
  bool needsLegacyAssociation() const { return associationRequired; }
  const Identity& conflictIdentity() const { return conflict; }
  TintaJournalResult eraseMatching(const char* path, uint32_t spineCount, std::vector<BookmarkEntry>& entries,
                                   Matcher matches, void* matchContext) {
    if (!matches || !initialize(path, spineCount)) return TintaJournalResult::Invalid;
    // Reconstruct the one-shot edit in one retained allocation; list strings
    // stay valid until the single publication/load after all explicit deletes.
    auto edit = makeUniqueNoThrow<std::optional<NativeBookmarkEditSession>>();
    if (!edit) return failure("delete batch allocation");
    bool changed = false;
    for (const auto& entry : entries) {
      if (!matches(matchContext, entry)) continue;
      if (!gate(context)) return failure("delete batch context");
      edit->emplace();
      const auto result = edit->value().erase(entry, edition, identities);
      if (result != TintaJournalResult::Ok) {
        recoveryRequired = changed || edit->value().requiresRecovery();
        if (result == TintaJournalResult::Conflict && !edit->value().requiresRecovery() && gate(context))
          conflict = entry.identity;
        return result;
      }
      changed = true;
    }
    edit.reset();
    if (!changed) {
      recoveryRequired = false;
      return TintaJournalResult::Unavailable;
    }
    return publishAndLoad(spineCount, entries);
  }

 private:
  TintaJournalResult recoverWorkspace(const char* alternate = nullptr) {
    auto publication = publisher();
    if (!publication) return failure("recovery allocation");
    auto result = publication->recoverPending(active.data());
    if ((result == TintaJournalResult::Invalid || result == TintaJournalResult::Corrupt) && alternate &&
        gate(context)) {
      publication.reset();
      publication = publisher();
      if (!publication) return failure("alternate recovery allocation");
      result = publication->recoverPending(alternate);
    }
    if (result != TintaJournalResult::Ok && result != TintaJournalResult::Unavailable) return result;
    publication.reset();
    auto orphan = makeUniqueNoThrow<NativeBookmarkPreparationSession>(preparationValidator, context);
    if (!orphan) return failure("orphan allocation");
    result = orphan->recover(active.data(), edition, generation);
    if (result == TintaJournalResult::Corrupt && alternate && gate(context)) {
      orphan.reset();
      orphan = makeUniqueNoThrow<NativeBookmarkPreparationSession>(preparationValidator, context);
      if (!orphan) return failure("alternate orphan allocation");
      result = orphan->recover(alternate, edition, generation);
    }
    return result == TintaJournalResult::Unavailable ? TintaJournalResult::Ok : result;
  }
  bool initialize(const char* path, uint32_t spineCount) {
    if (used || !path || !gate || !migrationValidator || !publicationValidator || !preparationValidator ||
        !spineCount || spineCount > 65536 || scratch.size() < 64 || !tinta_body_detail::nonzero(edition) ||
        !tinta_body_detail::nonzero(generation))
      return false;
    used = true;
    const auto size = strnlen(path, active.size());
    const std::string_view view(path, size);
    const auto parent = bookmarkCacheParent(view, edition);
    if (size == active.size() || !hal_filename::valid(view) || !parent || !gate(context)) return false;
    editionCache = parent == BOOKMARK_EDITION_CACHE_PARENT;
    lookup.emplace(nullptr, nullptr, parent);
    std::copy(view.begin(), view.end(), active.begin());
    active[size] = 0;
    recoveryRequired = true;
    return true;
  }
  bool randomTransaction() {
    return gate(context) && identities.randomIdentity(transaction) && tinta_body_detail::nonzero(transaction);
  }
  std::unique_ptr<NativeBookmarkPublicationSession> publisher() {
    return makeUniqueNoThrow<NativeBookmarkPublicationSession>(scratch, publicationValidator, context,
                                                               preparationValidator);
  }
  TintaJournalResult publishAndLoad(uint32_t spineCount, std::vector<BookmarkEntry>& entries) {
    if (!randomTransaction()) return failure("publication transaction");
    auto publication = publisher();
    if (!publication) return failure("publication allocation");
    const auto published =
        publication->publish(active.data(), edition, spineCount, transaction, generation, editionCache);
    if (published != TintaJournalResult::Ok && published != TintaJournalResult::Unavailable) {
      if (published == TintaJournalResult::Conflict && gate(context)) conflict = publication->conflictIdentity();
      return published;
    }
    publication.reset();
    if (!gate(context)) return failure("load context");
    const auto present = lookup->inspect(active.data());
    if (present == CompanionFilePresence::Error) return failure("load lookup");
    if (present == CompanionFilePresence::Present) {
      if (!BookmarkFile::loadFromPath(active.data(), entries)) return failure("cache load");
    } else {
      if (published == TintaJournalResult::Ok) return failure("published cache missing");
      entries.clear();
    }
    if (!gate(context)) return failure("loaded context");
    recoveryRequired = false;
    return TintaJournalResult::Ok;
  }
  static TintaJournalResult failure(const char* operation) {
    LOG_ERR("COMPANION", "Bookmark reader session %s failed", operation);
    return TintaJournalResult::IoError;
  }
  std::span<uint8_t> scratch;
  Digest edition;
  Identity generation, transaction{}, conflict{};
  IdentityStorage& identities;
  Gate gate;
  NativeBookmarkMigrationSession::Validator migrationValidator;
  NativeBookmarkPublicationSession::ContextValidator publicationValidator;
  NativeBookmarkPublicationSession::PreparationValidator preparationValidator;
  void* context;
  Release release;
  std::array<char, 512> active{};
  std::optional<HalCompanionFileLookup> lookup;
  bool used = false, recoveryRequired = false, editionCache = false, associationRequired = false;
};
}  // namespace companion
