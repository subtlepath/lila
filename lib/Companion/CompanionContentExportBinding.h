#pragma once

#include "CompanionContentHandoff.h"

namespace companion {
// Retained by the activity; admission follows source verification under its lock.
class ContentExportBinding final {
 public:
  void reset() {
    request = {};
    owner = {};
    revision = 0;
  }
  ContentReadResult admit(const ContentHandoffRequest& candidate, bool authorized, const Identity& installation,
                          const Identity& generation, uint64_t inventoryRevision, ContentReadResult verified) {
    if (!authorized || !content_read_detail::nonzero(installation)) return ContentReadResult::Unauthorized;
    if (!validContentHandoffRequest(candidate)) return ContentReadResult::Invalid;
    if (candidate.read.generation != generation) return ContentReadResult::WrongStorage;
    if (!inventoryRevision) return ContentReadResult::Busy;
    if (revision && (candidate.transaction != request.transaction || candidate.read != request.read ||
                     installation != owner || inventoryRevision != revision))
      return ContentReadResult::Busy;
    if (verified != ContentReadResult::Ok) return verified;
    request = candidate;
    owner = installation;
    revision = inventoryRevision;
    return ContentReadResult::Ok;
  }
  bool boundTo(const Identity& transaction, const Identity& installation, const Identity& generation,
               uint64_t inventoryRevision) const {
    return revision && revision == inventoryRevision && request.transaction == transaction && owner == installation &&
           request.read.generation == generation;
  }
  bool permits(const Identity& transaction, const Identity& installation, uint64_t inventoryRevision,
               const ContentReadRequest& read) const {
    return boundTo(transaction, installation, read.generation, inventoryRevision) && validContentReadRequest(read) &&
           read.manifest == request.read.manifest && read.offset >= request.read.offset;
  }

 private:
  ContentHandoffRequest request{};
  Identity owner{};
  uint64_t revision = 0;
};
}  // namespace companion
