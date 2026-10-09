#pragma once

#include "CompanionInventoryPaths.h"
#include "HalCompanionHeapAdmission.h"
#include "HalContentMetadataLookup.h"
#include "HalContentReadSource.h"

namespace companion {
// Retained outside the task stack. The caller supplies an opened, verified paths
// snapshot and excludes mutations while dispatching commands under its session lock.
class HalContentReadSession final {
 public:
  explicit HalContentReadSession(InventoryHashProgress progress = nullptr, void* context = nullptr)
      : source(progress, context) {}
  ~HalContentReadSession() { reset(); }
  bool prepare() {
    if (!admitCompanionHeap() || !file.prepareDirectoryEntry()) return false;
    if (!admitCompanionHeap()) {
      file = HalFile();
      return false;
    }
    return true;
  }
  bool reset() {
    source.detach();
    const bool closed = !file || file.close();
    if (!closed) LOG_ERR("COMPANION", "Content export handle close failed");
    owner = {};
    revision = 0;
    path[0] = 0;
    return closed;
  }
  size_t metadataReply(const ContentMetadataRequest& request, bool authorized, const Identity& generation,
                       uint64_t inventoryRevision, bool busy, InventoryPaths& paths, std::span<uint8_t> output) {
    return contentMetadataLookup(request, authorized, generation, inventoryRevision, busy, paths, path, output);
  }
  size_t reply(bool authorized, const Identity& installation, const Identity& generation, uint64_t inventoryRevision,
               bool busy, InventoryPaths& paths, std::span<const uint8_t> input, std::span<uint8_t> scratch,
               std::span<uint8_t> output) {
    if (!decodeContentReadRequest(input, request) ||
        output.size() < CONTENT_READ_REPLY_HEADER_SIZE + contentReadByteCount(request))
      return 0;
    if (!authorized || !content_read_detail::nonzero(installation))
      return failure(ContentReadResult::Unauthorized, output);
    if (request.generation != generation) return failure(ContentReadResult::WrongStorage, output);
    if (busy || !inventoryRevision || !admitCompanionHeap()) return failure(ContentReadResult::Busy, output);
    if (!paths.belongsTo(generation, inventoryRevision)) return failure(ContentReadResult::Corrupt, output);
    if (owner != installation || revision != inventoryRevision || !source.matches(request)) {
      if (!reset()) return failure(ContentReadResult::IoError, output);
      const auto found = paths.find(request.manifest, path);
      if (found != InventoryPathResult::Found)
        return failure(found == InventoryPathResult::Missing ? ContentReadResult::NotFound : ContentReadResult::Corrupt,
                       output);
      if (!Storage.openFileForReadReusing("COMPANION", path.data(), file))
        return failure(ContentReadResult::IoError, output);
      if (!admitCompanionHeap()) return failure(ContentReadResult::Busy, output);
      const auto attached = source.attach(request, file, scratch);
      if (attached != ContentReadResult::Ok) return failure(attached, output);
      owner = installation;
      revision = inventoryRevision;
    }
    auto bytes = output.subspan(CONTENT_READ_REPLY_HEADER_SIZE, contentReadByteCount(request));
    const auto result = source.read(request, bytes);
    if (result != ContentReadResult::Ok) return failure(result, output);
    return encodeContentReadReply(request, result, bytes, output);
  }

 private:
  ContentReadRequest request{};
  Identity owner{};
  uint64_t revision = 0;
  std::array<char, INVENTORY_PATH_LIMIT + 1> path{};
  HalFile file;
  HalContentReadSource source;
  size_t failure(ContentReadResult result, std::span<uint8_t> output) {
    reset();
    return encodeContentReadReply(request, result, {}, output);
  }
};
}  // namespace companion
