#pragma once

#include "HalBookmarkLegacyBackup.h"
#include "HalBookmarkMigrationPaths.h"
#include "HalBookmarkMigrationRecord.h"
#include "HalBookmarkPathHash.h"

namespace companion {
// Checked off-stack owner. Caller freezes bookmark writes and resolves the
// source path against the verified edition. Completed migration claims persist.
class HalBookmarkMigrationBackupSession final {
 public:
  using Validator = bool (*)(void*, const BookmarkMigrationClaim&);
  HalBookmarkMigrationBackupSession(std::span<uint8_t> scratch, Validator validator, void* context)
      : scratch(scratch), validator(validator), context(context) {
    mbedtls_sha256_init(&sha);
  }
  TintaJournalResult prepare(const char* source, const Digest& edition, const Identity& transaction,
                             const Identity& generation) {
    if (used || !source || !validator || !tinta_body_detail::nonzero(transaction) ||
        !tinta_body_detail::nonzero(generation) || !tinta_body_detail::nonzero(edition))
      return TintaJournalResult::Invalid;
    used = true;
    const auto sourceLength = strnlen(source, 512);
    const std::string_view sourcePath(source, sourceLength);
    static constexpr std::string_view PARENT = "/.crosspoint/bookmarks/";
    if (!validInventoryPath(sourcePath) || !hal_filename::valid(sourcePath) || !sourcePath.starts_with(PARENT) ||
        sourcePath.size() <= PARENT.size() || sourcePath.substr(PARENT.size()).find('/') != std::string_view::npos)
      return TintaJournalResult::Invalid;
    // FAT paths are case insensitive; aliases must use the same migration key.
    static constexpr uint8_t DOMAIN[] = "lila-bookmark-migration-path-v1";
    if (mbedtls_sha256_starts(&sha, 0) || mbedtls_sha256_update(&sha, DOMAIN, sizeof(DOMAIN) - 1))
      return failure("path SHA start");
    if (!updateBookmarkPathHash(sha, sourcePath)) return failure("path SHA update");
    if (mbedtls_sha256_finish(&sha, pathHash.data()) || !paths.initialize(edition, pathHash))
      return failure("path SHA finish");
    if (!Storage.ensureDirectoryExists(TRANSFER_DIRECTORY)) return failure("directory");
    auto pending = records.read(paths.record(), claim);
    if (pending == BookmarkPublicationRecord::Missing) pending = records.read(paths.recordTemporary(), claim);
    if (pending == BookmarkPublicationRecord::Error) return failure("claim read");
    if (pending == BookmarkPublicationRecord::Other) return TintaJournalResult::Corrupt;
    const bool recovering = pending == BookmarkPublicationRecord::Matches;
    if (!recovering) {
      claim.sourcePathHash = pathHash;
      claim.transaction = transaction;
      claim.storageGeneration = generation;
      claim.edition = edition;
      if (!Storage.openFileForReadReusing("COMPANION", source, file)) return failure("source open");
      const bool hashed =
          !file.isDirectory() && hashInventoryFile(file, scratch, claim.originalLength, claim.originalHash);
      const bool closed = close();
      if (!hashed || !closed) return failure("source hash/close");
    }
    if (claim.edition != edition || claim.sourcePathHash != pathHash || claim.storageGeneration != generation ||
        !validBookmarkMigrationClaim(claim) || !validator(context, claim) || !paths.bind(claim))
      return TintaJournalResult::Invalid;
    if (!recovering) {
      const auto backup = lookup.inspect(paths.backup());
      const auto temporary = lookup.inspect(paths.backupTemporary());
      if (backup == CompanionFilePresence::Error || temporary == CompanionFilePresence::Error)
        return failure("namespace lookup");
      if (backup != CompanionFilePresence::Missing || temporary != CompanionFilePresence::Missing)
        return TintaJournalResult::Corrupt;
    }
    if (!validator(context, claim)) return TintaJournalResult::Invalid;
    if (!records.persist(paths.record(), paths.recordTemporary(), claim)) return failure("claim persist");
    // Durable claim and the fresh absence preflight own this transaction namespace.
    if (recovering) {
      const auto temporary = lookup.inspect(paths.backupTemporary());
      if (temporary == CompanionFilePresence::Error) return failure("temporary lookup");
      if (temporary == CompanionFilePresence::Present) {
        if (!Storage.openFileForReadReusing("COMPANION", paths.backupTemporary(), file))
          return failure("temporary open");
        if (file.isDirectory()) {
          return close() ? TintaJournalResult::Corrupt : failure("directory close");
        }
        const bool hashed = hashInventoryFile(file, scratch, length, hash);
        const bool closed = close();
        if (!hashed || !closed) return failure("temporary hash/close");
        if (length != claim.originalLength || hash != claim.originalHash) {
          if (!validator(context, claim) || !Storage.remove(paths.backupTemporary()))
            return failure("owned partial remove");
        }
      }
    }
    if (!validator(context, claim)) return TintaJournalResult::Invalid;
    auto backup = makeUniqueNoThrow<HalBookmarkLegacyBackup>(scratch);
    if (!backup) return failure("backup allocation");
    if (!backup->create(source, paths.backup(), paths.backupTemporary(), claim.originalLength, claim.originalHash))
      return failure("backup creation");
    ready = true;
    return TintaJournalResult::Ok;
  }
  bool isPrepared() const { return ready; }
  const BookmarkMigrationClaim& migrationClaim() const { return claim; }
  const char* backupPath() const { return ready ? paths.backup() : nullptr; }
  ~HalBookmarkMigrationBackupSession() {
    close();
    mbedtls_sha256_free(&sha);
  }

 private:
  bool close() { return !file.isOpen() || file.close() ? true : logFailure("close"); }
  bool logFailure(const char* operation) {
    LOG_ERR("COMPANION", "Bookmark migration backup %s failed", operation);
    return false;
  }
  TintaJournalResult failure(const char* operation) {
    logFailure(operation);
    return TintaJournalResult::IoError;
  }
  std::span<uint8_t> scratch;
  Validator validator;
  void* context;
  HalBookmarkMigrationPaths paths;
  HalBookmarkMigrationRecord records;
  HalCompanionFileLookup lookup;
  HalFile file;
  BookmarkMigrationClaim claim{};
  mbedtls_sha256_context sha;
  Digest hash{}, pathHash{};
  uint64_t length = 0;
  bool used = false, ready = false;
};
}  // namespace companion
