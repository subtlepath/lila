#pragma once

#include "CompanionBookmarkLegacyImport.h"
#include "HalBookmarkMigrationBackupSession.h"
#include "util/BookmarkFile.h"

namespace companion {
// Checked off-stack owner. Caller excludes writers and verifies source/card/book
// association through the validator. Reuse the reader's loaded bookmark vector.
class NativeBookmarkMigrationSession final {
 public:
  using Validator = HalBookmarkMigrationBackupSession::Validator;
  using AnchorResolver = bool (*)(void*, BookmarkEntry&);
  NativeBookmarkMigrationSession(std::span<uint8_t> scratch, Validator validator, void* context)
      : scratch(scratch), validator(validator), context(context) {}
  TintaJournalResult importLegacy(const char* source, const Digest& edition, const Identity& transaction,
                                  const Identity& generation, uint32_t spineCount, IdentityStorage& identities,
                                  std::vector<BookmarkEntry>& entries, AnchorResolver resolver = nullptr,
                                  bool associateUnbound = false) {
    if (used || !validator || !spineCount || spineCount > 65536) return TintaJournalResult::Invalid;
    used = true;
    auto backup = makeUniqueNoThrow<HalBookmarkMigrationBackupSession>(scratch, validator, context);
    if (!backup) return failure("backup allocation");
    const auto prepared = backup->prepare(source, edition, transaction, generation);
    if (prepared != TintaJournalResult::Ok) return prepared;
    claim = backup->migrationClaim();
    const auto length = strnlen(backup->backupPath(), backupPath.size());
    if (length == backupPath.size()) return failure("backup path");
    memcpy(backupPath.data(), backup->backupPath(), length + 1);
    backup.reset();
    if (!BookmarkFile::loadFromPath(backupPath.data(), entries)) return failure("backup parse");
    // Verify the file again after parsing; malformed/changed bytes never authorize import.
    if (!validator(context, claim) || !verifyBackup()) return failure("parsed backup proof");
    for (auto& entry : entries) {
      if (!entry.hasVisibleTextOffset && (!resolver || !resolver(context, entry) || !entry.hasVisibleTextOffset))
        return TintaJournalResult::Invalid;
      vTaskDelay(1);
    }
    if (!validator(context, claim)) return TintaJournalResult::Invalid;
    auto importer = makeUniqueNoThrow<NativeBookmarkLegacyImport>();
    if (!importer) return failure("import allocation");
    if (associateUnbound) {
      const auto associated = importer->associateMissingIdentities(entries, claim.edition);
      if (associated != TintaJournalResult::Ok) {
        recoveryRequired = importer->requiresRecovery();
        if (associated == TintaJournalResult::Conflict && validator(context, claim))
          conflict = importer->conflictIdentity();
        return associated;
      }
      if (!validator(context, claim)) return TintaJournalResult::Invalid;
    }
    const auto result = importer->import(entries, claim.edition, claim.originalHash, spineCount, identities);
    recoveryRequired = importer->requiresRecovery();
    if (result == TintaJournalResult::Conflict && validator(context, claim)) conflict = importer->conflictIdentity();
    return result;
  }
  bool requiresRecovery() const { return recoveryRequired; }
  const Identity& conflictIdentity() const { return conflict; }
  const BookmarkMigrationClaim& migrationClaim() const { return claim; }

 private:
  bool verifyBackup() {
    if (!Storage.openFileForReadReusing("COMPANION", backupPath.data(), file)) return false;
    const bool hashed = !file.isDirectory() && hashInventoryFile(file, scratch, length, hash);
    const bool closed = file.close();
    return hashed && closed && length == claim.originalLength && hash == claim.originalHash;
  }
  TintaJournalResult failure(const char* operation) {
    LOG_ERR("COMPANION", "Bookmark migration %s failed", operation);
    return TintaJournalResult::IoError;
  }
  std::span<uint8_t> scratch;
  Validator validator;
  void* context;
  BookmarkMigrationClaim claim{};
  Identity conflict{};
  std::array<char, 192> backupPath{};
  HalFile file;
  Digest hash{};
  uint64_t length = 0;
  bool used = false, recoveryRequired = false;
};
}  // namespace companion
