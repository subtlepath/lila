#pragma once

#include "CompanionTintaAuthorityCheckpointPaths.h"
#include "HalTintaAuthorityCheckpointStore.h"

namespace companion {
// Retain off-stack while baseline publication excludes journal/checkpoint writers.
class HalTintaAuthorityCheckpoints final {
 public:
  HalTintaAuthorityCheckpoints() : records(target.data(), stage.data()) {}
  TintaAuthorityCheckpointResult persist(const TintaAuthorityCheckpoint& value) {
    if (!validTintaAuthorityCheckpoint(value) || !prepare(value.manifest))
      return TintaAuthorityCheckpointResult::Invalid;
    return records.persist(value);
  }
  TintaAuthorityCheckpointResult load(const Digest& manifestDigest, TintaAuthorityCheckpoint& output) {
    if (!prepare(manifestDigest)) return TintaAuthorityCheckpointResult::Invalid;
    const auto result = records.load(loaded);
    if (result != TintaAuthorityCheckpointResult::Ok) return result;
    if (loaded.manifest != manifestDigest) return TintaAuthorityCheckpointResult::Conflict;
    output = loaded;
    return result;
  }

 private:
  bool prepare(const Digest& digest) {
    return tintaAuthorityCheckpointPath(digest, false, target) && tintaAuthorityCheckpointPath(digest, true, stage);
  }
  std::array<char, TINTA_CHECKPOINT_PATH_SIZE> target{}, stage{};
  HalTintaAuthorityCheckpointStore records;
  TintaAuthorityCheckpoint loaded;
};
}  // namespace companion
