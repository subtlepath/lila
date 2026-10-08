#pragma once

#include "CompanionTintaAuthorityCheckpoint.h"
#include "HalCompanionFileLookup.h"

namespace companion {
enum class TintaAuthorityCheckpointResult { Ok, Missing, Invalid, Conflict, Corrupt, IoError };
// Borrowed companion-child paths must outlive this off-stack owner. The caller
// excludes mutation writers and audits authority before publishing a baseline checkpoint.
class HalTintaAuthorityCheckpointStore final {
 public:
  HalTintaAuthorityCheckpointStore(const char* path, const char* stage) : path(path), stage(stage) {}
  ~HalTintaAuthorityCheckpointStore() {
    if (file.isOpen() && !file.close()) error("destructor close");
  }
  TintaAuthorityCheckpointResult load(TintaAuthorityCheckpoint& output) {
    const auto result = read(path);
    if (result == TintaAuthorityCheckpointResult::Ok) output = decoded;
    return result;
  }
  TintaAuthorityCheckpointResult persist(const TintaAuthorityCheckpoint& value) {
    if (!path || !stage || strcmp(path, stage) == 0 || !encodeTintaAuthorityCheckpoint(value, expected))
      return TintaAuthorityCheckpointResult::Invalid;
    auto result = read(path);
    if (result == TintaAuthorityCheckpointResult::Ok)
      return bytes == expected ? result : TintaAuthorityCheckpointResult::Conflict;
    if (result != TintaAuthorityCheckpointResult::Missing) return result;
    result = read(stage);
    if (result == TintaAuthorityCheckpointResult::Ok && bytes != expected)
      return TintaAuthorityCheckpointResult::Conflict;
    if (result != TintaAuthorityCheckpointResult::Missing && result != TintaAuthorityCheckpointResult::Ok)
      return result;
    if (result == TintaAuthorityCheckpointResult::Missing) {
      if (!Storage.openFileForWriteReusing("COMPANION", stage, file)) return error("stage open");
      const bool written = !file.isDirectory() && file.seek64(0) &&
                           file.write(expected.data(), expected.size()) == expected.size() &&
                           file.truncate(expected.size()) && file.sync();
      const bool closed = file.close();
      if (!written || !closed) return error("stage write/sync/close");
    }
    if (!Storage.openFileForReadReusing("COMPANION", stage, file)) return error("stage durability open");
    const bool synced = !file.isDirectory() && file.sync();
    const bool closed = file.close();
    if (!synced || !closed) return error("stage durability sync/close");
    result = read(stage);
    if (result != TintaAuthorityCheckpointResult::Ok) return result;
    if (bytes != expected) return TintaAuthorityCheckpointResult::Conflict;
    result = read(path);
    if (result == TintaAuthorityCheckpointResult::Ok)
      return bytes == expected ? result : TintaAuthorityCheckpointResult::Conflict;
    if (result != TintaAuthorityCheckpointResult::Missing) return result;
    // A failed rename can have published the record; readback resolves that reply.
    const bool renamed = Storage.rename(stage, path);
    result = read(path);
    if (result == TintaAuthorityCheckpointResult::Ok)
      return bytes == expected ? result : TintaAuthorityCheckpointResult::Conflict;
    return renamed ? result : error("publication rename");
  }

 private:
  TintaAuthorityCheckpointResult read(const char* source) {
    if (!source || (file.isOpen() && !file.close()) || !Storage.ensureDirectoryExists(TRANSFER_DIRECTORY))
      return error("read preparation");
    const auto presence = lookup.inspect(source);
    if (presence == CompanionFilePresence::Missing) return TintaAuthorityCheckpointResult::Missing;
    if (presence == CompanionFilePresence::Error || !Storage.openFileForReadReusing("COMPANION", source, file))
      return error("lookup/open");
    const bool shape = !file.isDirectory() && file.fileSize64() == bytes.size();
    const bool loaded = shape && file.read(bytes.data(), bytes.size()) == static_cast<int>(bytes.size()) &&
                        file.fileSize64() == bytes.size();
    const bool closed = file.close();
    if (!closed || (shape && !loaded)) return error("read/close");
    if (!loaded || !decodeTintaAuthorityCheckpoint(bytes, decoded)) return TintaAuthorityCheckpointResult::Corrupt;
    return TintaAuthorityCheckpointResult::Ok;
  }
  static TintaAuthorityCheckpointResult error(const char* reason) {
    LOG_ERR("COMPANION", "Tinta authority checkpoint failed: %s", reason);
    return TintaAuthorityCheckpointResult::IoError;
  }
  const char* path;
  const char* stage;
  HalCompanionFileLookup lookup;
  HalFile file;
  std::array<uint8_t, TINTA_AUTHORITY_CHECKPOINT_SIZE> bytes{}, expected{};
  TintaAuthorityCheckpoint decoded;
};
}  // namespace companion
