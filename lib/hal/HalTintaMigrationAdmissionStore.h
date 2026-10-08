#pragma once

#include "CompanionTintaMigrationAdmission.h"
#include "HalCompanionFileLookup.h"

namespace companion {
enum class TintaMigrationAdmissionResult { Ok, Missing, Invalid, Conflict, Corrupt, IoError };
// Borrowed companion-child paths must outlive this off-stack owner. The caller
// excludes writers and verifies authenticated ownership and immutable backup/source
// hashes before recording the decision. This store alone does not admit migration.
class HalTintaMigrationAdmissionStore final {
 public:
  HalTintaMigrationAdmissionStore(const char* path, const char* stage) : path(path), stage(stage) {}
  ~HalTintaMigrationAdmissionStore() {
    if (file.isOpen() && !file.close()) error("destructor close");
  }
  TintaMigrationAdmissionResult load(TintaMigrationAdmission& output) {
    const auto result = read(path);
    if (result == TintaMigrationAdmissionResult::Ok) output = decoded;
    return result;
  }
  TintaMigrationAdmissionResult persist(const TintaMigrationAdmission& value) {
    if (!path || !stage || strcmp(path, stage) == 0 || !encodeTintaMigrationAdmission(value, expected))
      return TintaMigrationAdmissionResult::Invalid;
    auto result = read(path);
    if (result == TintaMigrationAdmissionResult::Ok)
      return bytes == expected ? result : TintaMigrationAdmissionResult::Conflict;
    if (result != TintaMigrationAdmissionResult::Missing) return result;
    result = read(stage);
    if (result == TintaMigrationAdmissionResult::Ok && bytes != expected)
      return TintaMigrationAdmissionResult::Conflict;
    if (result != TintaMigrationAdmissionResult::Missing && result != TintaMigrationAdmissionResult::Ok) return result;
    if (result == TintaMigrationAdmissionResult::Missing) {
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
    if (result != TintaMigrationAdmissionResult::Ok) return result;
    if (bytes != expected) return TintaMigrationAdmissionResult::Conflict;
    result = read(path);
    if (result == TintaMigrationAdmissionResult::Ok)
      return bytes == expected ? result : TintaMigrationAdmissionResult::Conflict;
    if (result != TintaMigrationAdmissionResult::Missing) return result;
    // A failed rename can have published the record; readback resolves that reply.
    const bool renamed = Storage.rename(stage, path);
    result = read(path);
    if (result == TintaMigrationAdmissionResult::Ok)
      return bytes == expected ? result : TintaMigrationAdmissionResult::Conflict;
    return renamed ? result : error("publication rename");
  }

 private:
  TintaMigrationAdmissionResult read(const char* source) {
    if (!source || (file.isOpen() && !file.close()) || !Storage.ensureDirectoryExists(TRANSFER_DIRECTORY))
      return error("read preparation");
    const auto presence = lookup.inspect(source);
    if (presence == CompanionFilePresence::Missing) return TintaMigrationAdmissionResult::Missing;
    if (presence == CompanionFilePresence::Error || !Storage.openFileForReadReusing("COMPANION", source, file))
      return error("lookup/open");
    const bool shape = !file.isDirectory() && file.fileSize64() == bytes.size();
    const bool loaded = shape && file.read(bytes.data(), bytes.size()) == static_cast<int>(bytes.size()) &&
                        file.fileSize64() == bytes.size();
    const bool closed = file.close();
    if (!closed || (shape && !loaded)) return error("read/close");
    if (!loaded || !decodeTintaMigrationAdmission(bytes, decoded)) return TintaMigrationAdmissionResult::Corrupt;
    return TintaMigrationAdmissionResult::Ok;
  }
  static TintaMigrationAdmissionResult error(const char* reason) {
    LOG_ERR("COMPANION", "Tinta migration admission failed: %s", reason);
    return TintaMigrationAdmissionResult::IoError;
  }
  const char* path;
  const char* stage;
  HalCompanionFileLookup lookup;
  HalFile file;
  std::array<uint8_t, TINTA_MIGRATION_ADMISSION_SIZE> bytes{}, expected{};
  TintaMigrationAdmission decoded;
};
}  // namespace companion
