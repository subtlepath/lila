#pragma once

#include "CompanionContentMetadata.h"
#include "CompanionInventoryPaths.h"
#include "HalCompanionHeapAdmission.h"

namespace companion {
// Path storage is retained by the caller; variable-size bytes use borrowed buffers.
inline size_t contentMetadataLookup(const ContentMetadataRequest& request, bool authorized, const Identity& generation,
                                    uint64_t revision, bool busy, InventoryPaths& paths, std::span<char> path,
                                    std::span<uint8_t> output) {
  const auto failure = [&](ContentReadResult result) {
    return encodeContentMetadataReply(request, result, {}, output);
  };
  if (!authorized) return failure(ContentReadResult::Unauthorized);
  if (request.generation != generation) return failure(ContentReadResult::WrongStorage);
  if (busy || !revision || !admitCompanionHeap()) return failure(ContentReadResult::Busy);
  if (!paths.belongsTo(generation, revision)) return failure(ContentReadResult::Corrupt);
  const auto found = paths.find(request.manifest, path);
  if (found != InventoryPathResult::Found)
    return failure(found == InventoryPathResult::Missing ? ContentReadResult::NotFound : ContentReadResult::Corrupt);
  const std::string_view fullPath(path.data());
  const auto name = fullPath.substr(fullPath.rfind('/') + 1);
  if (!validContentMetadataFilename(request.manifest, name)) return failure(ContentReadResult::Corrupt);
  return encodeContentMetadataReply(request, ContentReadResult::Ok, name, output);
}
}  // namespace companion
