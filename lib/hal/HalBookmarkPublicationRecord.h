#pragma once

#include "CompanionBookmarkPublicationRecord.h"
#include "HalCompanionFileLookup.h"

namespace companion {
// Checked off-stack owner; borrowed paths must be private companion children.
// Context validation and serialization belong to the publication coordinator.
class HalBookmarkPublicationRecord final {
 public:
  BookmarkPublicationRecord read(const char* path, BookmarkPublicationClaim& output) {
    if (!close()) return BookmarkPublicationRecord::Error;
    const auto presence = lookup.inspect(path);
    if (presence == CompanionFilePresence::Missing) return BookmarkPublicationRecord::Missing;
    if (presence == CompanionFilePresence::Error) return BookmarkPublicationRecord::Error;
    if (!Storage.openFileForReadReusing("COMPANION", path, file)) return error("record open");
    if (file.isDirectory() || file.fileSize64() != bytes.size()) {
      return close() ? BookmarkPublicationRecord::Other : BookmarkPublicationRecord::Error;
    }
    const bool readOk = file.read(bytes.data(), bytes.size()) == static_cast<int>(bytes.size());
    const bool closed = close();
    if (!readOk || !closed) return error("record read/close");
    return decodeBookmarkPublicationRecord(bytes, output) ? BookmarkPublicationRecord::Matches
                                                          : BookmarkPublicationRecord::Other;
  }
  BookmarkPublicationRecord inspect(const char* path, const BookmarkPublicationClaim& expected) {
    const auto result = read(path, decoded);
    return result == BookmarkPublicationRecord::Matches && decoded != expected ? BookmarkPublicationRecord::Other
                                                                               : result;
  }
  // Existing records are immutable. A valid same-claim temporary record can be
  // completed after interruption; foreign or partial bytes remain untouched.
  bool persist(const char* path, const char* temporary, const BookmarkPublicationClaim& claim) {
    if (!validBookmarkPublicationClaim(claim) || !path || !temporary || strcmp(path, temporary) == 0)
      return failure("record arguments");
    const auto active = inspect(path, claim);
    if (active == BookmarkPublicationRecord::Matches) return true;
    if (active != BookmarkPublicationRecord::Missing) return failure("record destination unavailable");
    const auto pending = inspect(temporary, claim);
    if (pending == BookmarkPublicationRecord::Missing) {
      if (!encodeBookmarkPublicationRecord(claim, bytes) ||
          !Storage.openFileForWriteReusing("COMPANION", temporary, file))
        return failure("record create");
      const bool written = !file.isDirectory() && file.write(bytes.data(), bytes.size()) == bytes.size() &&
                           file.sync() && file.fileSize64() == bytes.size();
      const bool closed = close();
      if (!written || !closed) return failure("record write/sync/close");
      if (inspect(temporary, claim) != BookmarkPublicationRecord::Matches) return failure("record readback");
    } else if (pending != BookmarkPublicationRecord::Matches) {
      return failure("record temporary unavailable");
    }
    // A previous failed sync may have left readable bytes without durability.
    file = Storage.open(temporary, O_RDWR);
    const bool synced = file && !file.isDirectory() && file.sync();
    const bool closed = close();
    if (!synced || !closed || inspect(temporary, claim) != BookmarkPublicationRecord::Matches)
      return failure("record retry sync/readback");
    if (inspect(path, claim) != BookmarkPublicationRecord::Missing || !Storage.rename(temporary, path))
      return failure("record rename");
    return inspect(path, claim) == BookmarkPublicationRecord::Matches ? true : failure("record installed readback");
  }
  bool remove(const char* path, const BookmarkPublicationClaim& claim) {
    const auto result = inspect(path, claim);
    if (result == BookmarkPublicationRecord::Missing) return true;
    if (result != BookmarkPublicationRecord::Matches || !Storage.remove(path)) return failure("record remove");
    return inspect(path, claim) == BookmarkPublicationRecord::Missing ? true : failure("record removal verification");
  }
  ~HalBookmarkPublicationRecord() { close(); }
  HalBookmarkPublicationRecord() = default;
  HalBookmarkPublicationRecord(const HalBookmarkPublicationRecord&) = delete;
  HalBookmarkPublicationRecord& operator=(const HalBookmarkPublicationRecord&) = delete;

 private:
  HalFile file;
  HalCompanionFileLookup lookup;
  std::array<uint8_t, BOOKMARK_PUBLICATION_RECORD_SIZE> bytes{};
  BookmarkPublicationClaim decoded{};
  bool close() { return !file.isOpen() || file.close() ? true : failure("record close"); }
  bool failure(const char* message) {
    LOG_ERR("COMPANION", "Bookmark publication %s", message);
    return false;
  }
  BookmarkPublicationRecord error(const char* message) {
    failure(message);
    return BookmarkPublicationRecord::Error;
  }
};
}  // namespace companion
