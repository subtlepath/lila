#pragma once

#include "HalVerifiedFileStage.h"

namespace companion {
// Checked off-stack owner. Caller freezes source/card context and proves private
// destination ownership before creation. Borrowed paths/scratch outlive the call.
class HalBookmarkLegacyBackup final {
 public:
  explicit HalBookmarkLegacyBackup(std::span<uint8_t> scratch) : stage(scratch), scratch(scratch) {}
  bool create(const char* source, const char* backup, const char* temporary, uint64_t expectedLength,
              const Digest& expectedHash) {
    if (used || !source || !backup || !temporary ||
        !std::any_of(expectedHash.begin(), expectedHash.end(), [](uint8_t byte) { return byte != 0; }) ||
        strcmp(source, backup) == 0 || strcmp(source, temporary) == 0 || strcmp(backup, temporary) == 0)
      return failure("arguments");
    used = true;
    if (!Storage.ensureDirectoryExists(TRANSFER_DIRECTORY)) return failure("directory");
    const auto existing = inspect(backup, expectedLength, expectedHash);
    if (existing == Presence::Matches) return true;
    if (existing != Presence::Missing) return failure("backup unavailable");
    const auto pending = inspect(temporary, expectedLength, expectedHash);
    if (pending == Presence::Missing) {
      if (!stage.begin(temporary, expectedLength) || !Storage.openFileForReadReusing("COMPANION", source, file))
        return failure("source/stage open");
      if (file.isDirectory() || file.fileSize64() != expectedLength) return failure("source extent");
      uint64_t at = 0;
      while (at < expectedLength) {
        const auto count = static_cast<size_t>(std::min<uint64_t>(scratch.size(), expectedLength - at));
        if (!count || file.read(scratch.data(), count) != static_cast<int>(count) ||
            !stage.write(at, scratch.first(count)))
          return failure("copy");
        at += count;
        vTaskDelay(1);
      }
      if (file.fileSize64() != expectedLength || !close() || !stage.seal(expectedLength, &expectedHash))
        return failure("source close/stage seal");
    } else if (pending != Presence::Matches) {
      return failure("temporary unavailable");
    }
    // Readable retry bytes still require durability before rename.
    file = Storage.open(temporary, O_RDWR);
    const bool synced = file && !file.isDirectory() && file.sync();
    const bool closed = close();
    if (!synced || !closed || inspect(temporary, expectedLength, expectedHash) != Presence::Matches ||
        lookup.inspect(backup) != CompanionFilePresence::Missing || !Storage.rename(temporary, backup))
      return failure("install");
    return inspect(backup, expectedLength, expectedHash) == Presence::Matches ? true : failure("installed hash");
  }
  ~HalBookmarkLegacyBackup() { close(); }

 private:
  enum class Presence { Missing, Matches, Other, Error };
  Presence inspect(const char* path, uint64_t expectedLength, const Digest& expectedHash) {
    if (!close()) return Presence::Error;
    const auto presence = lookup.inspect(path);
    if (presence == CompanionFilePresence::Missing) return Presence::Missing;
    if (presence == CompanionFilePresence::Error) return Presence::Error;
    if (!Storage.openFileForReadReusing("COMPANION", path, file)) return Presence::Error;
    const bool hashed = !file.isDirectory() && hashInventoryFile(file, scratch, length, hash);
    const bool closed = close();
    if (!hashed || !closed) return Presence::Error;
    return length == expectedLength && hash == expectedHash ? Presence::Matches : Presence::Other;
  }
  bool close() { return !file.isOpen() || file.close() ? true : failure("close"); }
  bool failure(const char* operation) {
    LOG_ERR("COMPANION", "Legacy bookmark backup %s failed", operation);
    return false;
  }
  HalVerifiedFileStage stage;
  HalCompanionFileLookup lookup;
  HalFile file;
  std::span<uint8_t> scratch;
  Digest hash{};
  uint64_t length = 0;
  bool used = false;
};
}  // namespace companion
