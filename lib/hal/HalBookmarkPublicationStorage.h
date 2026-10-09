#pragma once

#include "HalBookmarkPublicationRecord.h"
#include "HalInventoryFileHash.h"

namespace companion {
struct BookmarkPublicationPaths {
  const char* active;
  const char* candidate;
  const char* backup;
  const char* intent;
  const char* intentTemporary;
  const char* receipt;
  const char* receiptTemporary;
  const char* activeParent;
};
// Serialized checked off-stack owner. Paths, scratch and callback context are
// borrowed. Context callback verifies card generation, edition and path binding.
class HalBookmarkPublicationStorage final : public BookmarkPublicationStorage {
 public:
  using Validator = bool (*)(void*, const BookmarkPublicationClaim&);
  HalBookmarkPublicationStorage(BookmarkPublicationPaths paths, std::span<uint8_t> scratch, Validator contextValidator,
                                Validator authorityValidator, void* context)
      : paths(paths),
        scratch(scratch),
        activeLookup(nullptr, nullptr, paths.activeParent),
        contextValidator(contextValidator),
        authorityValidator(authorityValidator),
        context(context) {}
  bool validateContext(const BookmarkPublicationClaim& claim) override {
    if (!contextValidator || !scratch.size() || !paths.activeParent) return false;
    const char* values[] = {paths.active,          paths.candidate, paths.backup,          paths.intent,
                            paths.intentTemporary, paths.receipt,   paths.receiptTemporary};
    for (unsigned i = 0; i < 7; ++i) {
      if (!values[i] || !*values[i]) return false;
      for (unsigned j = 0; j < i; ++j)
        if (strcasecmp(values[i], values[j]) == 0) return false;
    }
    return contextValidator(context, claim);
  }
  bool validateAuthority(const BookmarkPublicationClaim& claim) override {
    return authorityValidator && authorityValidator(context, claim);
  }
  BookmarkPublicationRecord intent(const BookmarkPublicationClaim& claim) override {
    return records.inspect(paths.intent, claim);
  }
  BookmarkPublicationRecord receipt(const BookmarkPublicationClaim& claim) override {
    return records.inspect(paths.receipt, claim);
  }
  BookmarkPublicationFile file(BookmarkPublicationRole role, const BookmarkPublicationClaim& claim) override {
    if (!close()) return BookmarkPublicationFile::Error;
    auto& lookup = role == BookmarkPublicationRole::Active ? activeLookup : privateLookup;
    const auto presence = lookup.inspect(path(role));
    if (presence == CompanionFilePresence::Missing) return BookmarkPublicationFile::Missing;
    if (presence == CompanionFilePresence::Error) return BookmarkPublicationFile::Error;
    if (!Storage.openFileForReadReusing("COMPANION", path(role), handle)) return fileError("open");
    if (handle.isDirectory()) return close() ? BookmarkPublicationFile::Other : BookmarkPublicationFile::Error;
    const bool hashed = hashInventoryFile(handle, scratch, length, hash);
    const bool closed = close();
    if (!hashed || !closed) return fileError("hash/close");
    if (length == claim.candidateLength && hash == claim.candidateHash) return BookmarkPublicationFile::Candidate;
    if (claim.hadOriginal && length == claim.originalLength && hash == claim.originalHash)
      return BookmarkPublicationFile::Original;
    return BookmarkPublicationFile::Other;
  }
  // Capture the previous regular file descriptor without changing the claim on I/O failure.
  bool describeOriginal(BookmarkPublicationClaim& claim) {
    if (!close()) return false;
    const auto presence = activeLookup.inspect(paths.active);
    if (presence == CompanionFilePresence::Error) return failure("original lookup");
    if (presence == CompanionFilePresence::Missing) {
      claim.hadOriginal = false;
      claim.originalLength = 0;
      claim.originalHash = {};
      return true;
    }
    if (!Storage.openFileForReadReusing("COMPANION", paths.active, handle)) return failure("original open");
    const bool hashed = !handle.isDirectory() && hashInventoryFile(handle, scratch, length, hash);
    const bool closed = close();
    if (!hashed || !closed) return failure("original hash/close");
    claim.hadOriginal = true;
    claim.originalLength = length;
    claim.originalHash = hash;
    return true;
  }
  bool persistIntent(const BookmarkPublicationClaim& claim) override {
    return records.persist(paths.intent, paths.intentTemporary, claim);
  }
  bool move(BookmarkPublicationRole from, BookmarkPublicationRole to) override {
    return from != to && Storage.rename(path(from), path(to)) ? true : failure("rename");
  }
  bool remove(BookmarkPublicationRole role) override { return Storage.remove(path(role)) ? true : failure("remove"); }
  bool commitReceipt(const BookmarkPublicationClaim& claim) override {
    return records.persist(paths.receipt, paths.receiptTemporary, claim);
  }
  bool clearIntent() override {
    const auto result = records.read(paths.intent, pending);
    if (result == BookmarkPublicationRecord::Missing) return true;
    return result == BookmarkPublicationRecord::Matches && records.remove(paths.intent, pending)
               ? true
               : failure("clear intent");
  }
  ~HalBookmarkPublicationStorage() override { close(); }

 private:
  const char* path(BookmarkPublicationRole role) const {
    switch (role) {
      case BookmarkPublicationRole::Active:
        return paths.active;
      case BookmarkPublicationRole::Candidate:
        return paths.candidate;
      case BookmarkPublicationRole::Backup:
        return paths.backup;
    }
    return nullptr;
  }
  bool close() { return !handle.isOpen() || handle.close() ? true : failure("close"); }
  bool failure(const char* operation) {
    LOG_ERR("COMPANION", "Bookmark publication file %s failed", operation);
    return false;
  }
  BookmarkPublicationFile fileError(const char* operation) {
    failure(operation);
    return BookmarkPublicationFile::Error;
  }
  BookmarkPublicationPaths paths;
  std::span<uint8_t> scratch;
  HalCompanionFileLookup activeLookup, privateLookup;
  HalBookmarkPublicationRecord records;
  HalFile handle;
  Digest hash{};
  uint64_t length = 0;
  BookmarkPublicationClaim pending{};
  Validator contextValidator, authorityValidator;
  void* context;
};
}  // namespace companion
