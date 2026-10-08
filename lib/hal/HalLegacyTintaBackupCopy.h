#pragma once

#include <algorithm>

#include "HalVerifiedFileStage.h"

namespace companion {
// Checked heap workspace; paths/parent/scratch outlive calls and caller excludes source writers.
class HalLegacyTintaBackupCopy {
 public:
  HalLegacyTintaBackupCopy(const char* parent, std::span<uint8_t> scratch, InventoryHashProgress progress = nullptr,
                           void* context = nullptr)
      : scratch(scratch),
        progress(progress),
        context(context),
        lookup(progress, context, parent),
        stage(scratch, progress, context, parent) {}
  ~HalLegacyTintaBackupCopy() {
    if (source.isOpen() && !source.close()) failure("source close");
  }
  bool copy(const char* original, const char* candidate, const char* backup, uint64_t length, const Digest& hash) {
    if (!validSourcePath(original) || !validSourcePath(candidate) || !validSourcePath(backup) ||
        samePath(original, candidate) || samePath(original, backup) || samePath(candidate, backup) ||
        scratch.size() < 64 || std::none_of(hash.begin(), hash.end(), [](uint8_t byte) { return byte != 0; }) ||
        !stage.cleanup() || (source.isOpen() && !source.close()))
      return failure("arguments/close");
    const auto destination = lookup.inspect(backup);
    if (destination == CompanionFilePresence::Error) return failure("destination lookup");
    if (destination == CompanionFilePresence::Present) return verify(backup, length, hash);
    const auto pending = lookup.inspect(candidate);
    if (pending == CompanionFilePresence::Error) return failure("candidate lookup");
    if (pending == CompanionFilePresence::Present) {
      if (!syncCandidate(candidate, length, hash)) return false;
      return Storage.rename(candidate, backup) || failure("candidate publication");
    }
    if (!Storage.openFileForReadReusing("COMPANION", original, source) || !matches(source, length, hash) ||
        !source.seek64(0) || !stage.begin(candidate, length))
      return cleanupFailure("source/stage");
    for (uint64_t at = 0; at < length;) {
      const auto count = static_cast<size_t>(std::min<uint64_t>(scratch.size(), length - at));
      if ((progress && !progress(context)) || source.fileSize64() != length ||
          source.read(scratch.data(), count) != static_cast<int>(count) || !stage.write(at, scratch.first(count))) {
        return cleanupFailure("copy");
      }
      at += count;
    }
    if (!stage.seal(length, &hash) || !matches(source, length, hash) || !source.close())
      return cleanupFailure("seal/source recheck");
    return Storage.rename(candidate, backup) || failure("backup publication");
  }

 private:
  friend class HalLegacyTintaBackupCapture;
  bool discardOwnedPartial(const char* original, const char* candidate, const char* backup, uint64_t length,
                           const Digest& hash) {
    const auto destination = lookup.inspect(backup);
    if (destination == CompanionFilePresence::Present) return true;
    if (destination == CompanionFilePresence::Error) return failure("partial destination lookup");
    const auto pending = lookup.inspect(candidate);
    if (pending == CompanionFilePresence::Missing) return true;
    if (pending != CompanionFilePresence::Present) return failure("partial lookup");
    HalFile partial;
    if (!Storage.openFileForRead("COMPANION", candidate, partial) || partial.isDirectory())
      return failure("partial open");
    const auto extent = partial.fileSize64();
    if (extent >= length) return true;
    if (scratch.size() < 64 || !Storage.openFileForReadReusing("COMPANION", original, source) ||
        !matches(source, length, hash) || !source.seek64(0))
      return failure("partial source binding");
    const auto half = scratch.size() / 2;
    uint8_t reads = 0;
    for (uint64_t at = 0; at < extent;) {
      const auto count = static_cast<size_t>(std::min<uint64_t>(half, extent - at));
      if ((progress && !progress(context)) || partial.fileSize64() != extent ||
          partial.read(scratch.data(), count) != static_cast<int>(count) ||
          source.read(scratch.data() + half, count) != static_cast<int>(count) ||
          memcmp(scratch.data(), scratch.data() + half, count) != 0)
        return failure("partial prefix");
      at += count;
      if (++reads == 32) {
        reads = 0;
        vTaskDelay(1);
      }
    }
    if (!matches(source, length, hash) || partial.fileSize64() != extent || !source.close() || !partial.close())
      return failure("partial recheck/close");
    return Storage.remove(candidate) || failure("partial removal");
  }
  bool cleanupFailure(const char* operation) {
    stage.abort();
    if (source.isOpen() && !source.close()) failure("source cleanup close");
    return failure(operation);
  }
  static bool validSourcePath(const char* path) {
    if (!path) return false;
    const auto length = strnlen(path, 512);
    const std::string_view view(path, length);
    if (length == 512 || !validInventoryPath(view)) return false;
    for (size_t at = 0; at < view.size(); ++at) {
      const auto byte = static_cast<unsigned char>(view[at]);
      if (!((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') ||
            byte == '/' || byte == '.' || byte == '_' || byte == '-'))
        return false;
      if (byte == '.' && (at + 1 == view.size() || view[at + 1] == '/')) return false;
    }
    return true;
  }
  static bool samePath(const char* a, const char* b) {
    const auto lower = [](unsigned char value) { return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value; };
    while (*a && *b && lower(*a) == lower(*b)) {
      ++a;
      ++b;
    }
    return *a == 0 && *b == 0;
  }
  bool matches(HalFile& file, uint64_t length, const Digest& hash) {
    uint64_t actualLength = 0;
    Digest actualHash{};
    return file.isOpen() && !file.isDirectory() && file.fileSize64() == length &&
           hashInventoryFile(file, scratch, actualLength, actualHash, progress, context) && actualLength == length &&
           actualHash == hash;
  }
  bool verify(const char* path, uint64_t length, const Digest& hash) {
    HalFile file;
    if (!Storage.openFileForRead("COMPANION", path, file) || !matches(file, length, hash))
      return failure("immutable backup binding");
    return true;
  }
  bool syncCandidate(const char* path, uint64_t length, const Digest& hash) {
    HalFile file = Storage.open(path, O_RDWR);
    if (!matches(file, length, hash) || !file.sync() || !matches(file, length, hash))
      return failure("candidate sync/readback");
    return true;
  }
  static bool failure(const char* operation) {
    LOG_ERR("COMPANION", "Legacy backup copy failed: %s", operation);
    return false;
  }
  std::span<uint8_t> scratch;
  InventoryHashProgress progress;
  void* context;
  HalFile source;
  HalCompanionFileLookup lookup;
  HalVerifiedFileStage stage;
};
}  // namespace companion
