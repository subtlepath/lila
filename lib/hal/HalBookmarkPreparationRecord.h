#pragma once

#include "CompanionBookmarkPreparationClaim.h"
#include "HalCompanionFileLookup.h"

namespace companion {
// Checked off-stack immutable record owner. Caller serializes preparation and
// verifies context/path ownership; paths must be private companion children.
class HalBookmarkPreparationRecord final {
 public:
  BookmarkPublicationRecord read(const char* path, BookmarkPreparationClaim& output) {
    if (!close()) return BookmarkPublicationRecord::Error;
    const auto presence = lookup.inspect(path);
    if (presence == CompanionFilePresence::Missing) return BookmarkPublicationRecord::Missing;
    if (presence == CompanionFilePresence::Error) return BookmarkPublicationRecord::Error;
    if (!Storage.openFileForReadReusing("COMPANION", path, file)) return error("open");
    if (file.isDirectory() || file.fileSize64() != bytes.size())
      return close() ? BookmarkPublicationRecord::Other : BookmarkPublicationRecord::Error;
    const bool read = file.read(bytes.data(), bytes.size()) == static_cast<int>(bytes.size());
    const bool closed = close();
    if (!read || !closed) return error("read/close");
    return decodeBookmarkPreparationClaim(bytes, output) ? BookmarkPublicationRecord::Matches
                                                         : BookmarkPublicationRecord::Other;
  }
  BookmarkPublicationRecord inspect(const char* path, const BookmarkPreparationClaim& expected) {
    const auto result = read(path, decoded);
    return result == BookmarkPublicationRecord::Matches && decoded != expected ? BookmarkPublicationRecord::Other
                                                                               : result;
  }
  bool persist(const char* path, const char* temporary, const BookmarkPreparationClaim& claim) {
    if (!validBookmarkPreparationClaim(claim) || !path || !temporary || strcasecmp(path, temporary) == 0)
      return failure("arguments");
    const auto active = inspect(path, claim);
    if (active == BookmarkPublicationRecord::Matches) return true;
    if (active != BookmarkPublicationRecord::Missing) return failure("destination unavailable");
    const auto pending = inspect(temporary, claim);
    if (pending == BookmarkPublicationRecord::Missing) {
      if (!encodeBookmarkPreparationClaim(claim, bytes) ||
          !Storage.openFileForWriteReusing("COMPANION", temporary, file))
        return failure("create");
      const bool written = !file.isDirectory() && file.write(bytes.data(), bytes.size()) == bytes.size() &&
                           file.sync() && file.fileSize64() == bytes.size();
      const bool closed = close();
      if (!written || !closed) return failure("write/sync/close");
    } else if (pending != BookmarkPublicationRecord::Matches)
      return failure("temporary unavailable");
    file = Storage.open(temporary, O_RDWR);
    const bool synced = file && !file.isDirectory() && file.sync();
    const bool closed = close();
    if (!synced || !closed || inspect(temporary, claim) != BookmarkPublicationRecord::Matches ||
        inspect(path, claim) != BookmarkPublicationRecord::Missing || !Storage.rename(temporary, path))
      return failure("sync/readback/rename");
    return inspect(path, claim) == BookmarkPublicationRecord::Matches ? true : failure("installed readback");
  }
  bool remove(const char* path, const BookmarkPreparationClaim& claim) {
    const auto present = inspect(path, claim);
    if (present == BookmarkPublicationRecord::Missing) return true;
    if (present != BookmarkPublicationRecord::Matches || !Storage.remove(path)) return failure("remove ownership");
    return inspect(path, claim) == BookmarkPublicationRecord::Missing ? true : failure("remove readback");
  }
  ~HalBookmarkPreparationRecord() { close(); }

 private:
  bool close() { return !file.isOpen() || file.close() ? true : failure("close"); }
  bool failure(const char* operation) {
    LOG_ERR("COMPANION", "Bookmark preparation claim %s failed", operation);
    return false;
  }
  BookmarkPublicationRecord error(const char* operation) {
    failure(operation);
    return BookmarkPublicationRecord::Error;
  }
  HalFile file;
  HalCompanionFileLookup lookup;
  std::array<uint8_t, BOOKMARK_PREPARATION_CLAIM_SIZE> bytes{};
  BookmarkPreparationClaim decoded{};
};
}  // namespace companion
