#pragma once

#include "CompanionContentRemovalJournal.h"
#include "CompanionRemovalMetadataSnapshot.h"
#include "HalCompanionFileLookup.h"

namespace companion {
enum class RemovalMetadataStorageResult { Ok, Missing, Invalid, Conflict, Corrupt, IoError };
// Encoding/readback scratch (240 bytes) is session-owned and disjoint from the
// declaration and journal. No JSON bytes or published metadata are modified here.
class HalRemovalMetadataSnapshotStorage final {
 public:
  explicit HalRemovalMetadataSnapshotStorage(std::span<uint8_t> scratch) : scratch(scratch) {}
  ~HalRemovalMetadataSnapshotStorage() { close(); }
  RemovalMetadataStorageResult load(const Digest& planHash, RemovalMetadataFile kind, RemovalMetadataSnapshot& output) {
    if (scratch.size() < REMOVAL_METADATA_SNAPSHOT_SIZE || !removalMetadataDigestNonzero(planHash) ||
        (kind != RemovalMetadataFile::State && kind != RemovalMetadataFile::Recent))
      return RemovalMetadataStorageResult::Invalid;
    if (!prepare(planHash, kind)) return error("load preparation");
    const auto result = read(target.data());
    if (result != RemovalMetadataStorageResult::Ok) return result;
    if (decoded.planHash != planHash || decoded.file != kind) return RemovalMetadataStorageResult::Conflict;
    output = decoded;
    return RemovalMetadataStorageResult::Ok;
  }
  RemovalMetadataStorageResult persist(const RemovalMetadataSnapshot& declaration, ContentRemovalJournal& owner) {
    if (scratch.size() < REMOVAL_METADATA_SNAPSHOT_SIZE || !validRemovalMetadataSnapshot(declaration) ||
        !owner.current() || owner.current()->phase != ContentRemovalPhase::Quarantined ||
        owner.current()->request != declaration.request || owner.current()->planHash != declaration.planHash)
      return RemovalMetadataStorageResult::Invalid;
    expected = declaration;
    checkpoint = *owner.current();
    journal = &owner;
    if (!prepare(expected.planHash, expected.file) || !guard()) return error("persist preparation");
    auto result = read(target.data());
    if (!guard()) return RemovalMetadataStorageResult::Conflict;
    if (result == RemovalMetadataStorageResult::Ok)
      return decoded == expected ? result : RemovalMetadataStorageResult::Conflict;
    if (result != RemovalMetadataStorageResult::Missing) return result;
    // The exact quarantined owner may rebuild its bounded unpublished stage.
    // A complete foreign declaration or any directory remains untouched.
    result = read(stage.data());
    if (!guard()) return RemovalMetadataStorageResult::Conflict;
    if (result == RemovalMetadataStorageResult::Ok && decoded != expected)
      return RemovalMetadataStorageResult::Conflict;
    if (result != RemovalMetadataStorageResult::Missing && result != RemovalMetadataStorageResult::Corrupt &&
        result != RemovalMetadataStorageResult::Ok)
      return result;
    if (encodeRemovalMetadataSnapshot(expected, scratch) != REMOVAL_METADATA_SNAPSHOT_SIZE || !guard() ||
        !Storage.openFileForWriteReusing("COMPANION", stage.data(), file))
      return error("stage open/encode");
    const bool written = !file.isDirectory() &&
                         file.write(scratch.data(), REMOVAL_METADATA_SNAPSHOT_SIZE) == REMOVAL_METADATA_SNAPSHOT_SIZE &&
                         file.truncate(REMOVAL_METADATA_SNAPSHOT_SIZE) && file.sync();
    const bool closed = close();
    if (!written || !closed || !guard()) return error("stage write/sync/ownership");
    result = read(stage.data());
    if (result != RemovalMetadataStorageResult::Ok || decoded != expected || !guard()) return error("stage readback");
    if (lookup.inspect(target.data()) != CompanionFilePresence::Missing || !guard() ||
        !Storage.rename(stage.data(), target.data()))
      return error("publication rename");
    result = read(target.data());
    if (result != RemovalMetadataStorageResult::Ok || decoded != expected || !guard())
      return error("publication readback");
    return RemovalMetadataStorageResult::Ok;
  }

 private:
  std::span<uint8_t> scratch;
  RemovalMetadataSnapshot expected, decoded;
  ContentRemovalRecord checkpoint;
  ContentRemovalJournal* journal = nullptr;
  HalCompanionFileLookup lookup;
  HalFile file;
  std::array<char, 112> target{}, stage{};
  bool guard() const { return journal && journal->current() && *journal->current() == checkpoint; }
  bool close() { return !file.isOpen() || file.close() || failure("close"); }
  bool prepare(const Digest& planHash, RemovalMetadataFile kind) {
    if (scratch.size() < REMOVAL_METADATA_SNAPSHOT_SIZE || !removalMetadataDigestNonzero(planHash) ||
        (kind != RemovalMetadataFile::State && kind != RemovalMetadataFile::Recent) || !close() || !Storage.ready() ||
        !Storage.ensureDirectoryExists(TRANSFER_DIRECTORY))
      return false;
    static constexpr char PREFIX[] = "/.crosspoint/companion/removal-snapshot-";
    static constexpr char HEX_DIGITS[] = "0123456789abcdef";
    static_assert(sizeof(PREFIX) + 64 + 1 + 4 <= 112);
    size_t at = sizeof(PREFIX) - 1;
    std::copy_n(PREFIX, at, target.begin());
    for (const auto byte : planHash) {
      target[at++] = HEX_DIGITS[byte >> 4];
      target[at++] = HEX_DIGITS[byte & 15];
    }
    target[at++] = kind == RemovalMetadataFile::State ? 's' : 'r';
    target[at] = 0;
    std::copy_n(target.begin(), at, stage.begin());
    std::copy_n(".tmp", 5, stage.begin() + at);
    return true;
  }
  RemovalMetadataStorageResult read(const char* path) {
    if (!close()) return error("read close");
    const auto presence = lookup.inspect(path);
    if (presence == CompanionFilePresence::Missing) return RemovalMetadataStorageResult::Missing;
    if (presence != CompanionFilePresence::Present || !Storage.openFileForReadReusing("COMPANION", path, file))
      return error("read open");
    if (file.isDirectory() || file.fileSize64() > REMOVAL_METADATA_SNAPSHOT_SIZE) {
      close();
      return error("read collision");
    }
    const bool exact = file.fileSize64() == REMOVAL_METADATA_SNAPSHOT_SIZE;
    if (exact &&
        file.read(scratch.data(), REMOVAL_METADATA_SNAPSHOT_SIZE) != static_cast<int>(REMOVAL_METADATA_SNAPSHOT_SIZE)) {
      close();
      return error("read bytes");
    }
    const bool valid = exact && decodeRemovalMetadataSnapshot(scratch.first(REMOVAL_METADATA_SNAPSHOT_SIZE), decoded);
    const bool synced = valid && file.sync();
    const bool closed = close();
    if (!closed || (valid && !synced)) return error("read sync/close");
    return valid ? RemovalMetadataStorageResult::Ok : RemovalMetadataStorageResult::Corrupt;
  }
  static bool failure(const char* reason) {
    LOG_ERR("COMPANION", "Removal snapshot storage failed: %s", reason);
    return false;
  }
  static RemovalMetadataStorageResult error(const char* reason) {
    failure(reason);
    return RemovalMetadataStorageResult::IoError;
  }
};
}  // namespace companion
