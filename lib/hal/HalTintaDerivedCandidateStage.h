#pragma once

#include "CompanionTintaDerivedPaths.h"
#include "CompanionTintaReceiveCheckpoint.h"
#include "HalVerifiedFileStage.h"

namespace companion {
// Session-owned fixed paths/retained handles exceed the task-local budget.
// Caller excludes state writers and pending publication before beginning.
class HalTintaDerivedCandidateStage {
 public:
  explicit HalTintaDerivedCandidateStage(std::span<uint8_t> scratch) : stage(scratch, nullptr, nullptr, root.data()) {}
  bool begin(const TintaDerivedManifestView& manifest, TintaDerivedFile file, const Identity& course,
             const Identity& storage, const Digest& pack, const Digest& frontier) {
    valid = sealed = false;
    if (!stage.cleanup()) return failure("cleanup");
    if (!manifest.matches(course, storage, pack, frontier) || static_cast<unsigned>(file) >= 5 ||
        !courseStateDirectory(course, root) || !tintaDerivedFilePath(course, file, TintaDerivedRole::Candidate, path))
      return failure("binding or path");
    if (!Storage.ensureDirectoryExists(root.data())) return failure("course directory");
    expectedLength = manifest.length(file);
    std::copy(manifest.hash(file).begin(), manifest.hash(file).end(), expectedHash.begin());
    valid = stage.begin(path.data(), expectedLength);
    return valid;
  }
  // Checkpoint must come from recovered durable journal; caller excludes publication/writers.
  bool resume(const TintaReceiveCheckpoint& checkpoint, const TintaDerivedManifestView& manifest,
              const Digest& manifestHash, const Identity& transaction, const Identity& owner, const Identity& course,
              const Identity& storage, const Digest& pack, const Digest& frontier) {
    valid = sealed = false;
    if (!stage.cleanup()) return failure("resume cleanup");
    if (!matchesTintaReceiveCheckpoint(checkpoint, manifest, manifestHash, transaction, owner, course, storage, pack,
                                       frontier) ||
        !courseStateDirectory(course, root) ||
        !tintaDerivedFilePath(course, checkpoint.file, TintaDerivedRole::Candidate, path))
      return failure("resume binding");
    expectedLength = manifest.length(checkpoint.file);
    std::copy(manifest.hash(checkpoint.file).begin(), manifest.hash(checkpoint.file).end(), expectedHash.begin());
    valid = stage.resume(path.data(), expectedLength, checkpoint.offset);
    return valid;
  }
  bool write(uint64_t at, std::span<const uint8_t> bytes) {
    if (!valid || !stage.write(at, bytes)) return failure("write");
    return true;
  }
  bool syncPending(uint64_t& durableOffset) {
    if (!valid || !stage.syncPending(durableOffset)) return failure("pending sync");
    return true;
  }
  bool retainPending(uint64_t durableOffset) {
    valid = false;
    return stage.retainPending(durableOffset);
  }
  bool seal() {
    if (!valid || !stage.seal(expectedLength, &expectedHash)) return failure("seal");
    valid = false;
    sealed = true;
    return true;
  }
  bool isSealed() const { return sealed; }
  void abort() {
    valid = false;
    stage.abort();
  }

 private:
  bool failure(const char* reason) {
    valid = false;
    LOG_ERR("COMPANION", "Tinta candidate stage %s failed", reason);
    return false;
  }
  std::array<char, COURSE_STATE_DIRECTORY_SIZE> root{};
  std::array<char, COURSE_STATE_PATH_SIZE> path{};
  Digest expectedHash{};
  uint64_t expectedLength = 0;
  HalVerifiedFileStage stage;
  bool valid = false, sealed = false;
};
}  // namespace companion
