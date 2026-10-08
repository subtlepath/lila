#pragma once

#include "CompanionLegacyTintaBackupManifest.h"
#include "HalVerifiedFileStage.h"

namespace companion {
// Checked heap owner. Caller verifies backup files, prepares parent, excludes writers and retains borrowed paths.
class HalLegacyTintaBackupManifestStore {
 public:
  HalLegacyTintaBackupManifestStore(const char* parent, const char* path, const char* stagePath)
      : path(path), stagePath(stagePath), lookup(nullptr, nullptr, parent), stage(scratch, nullptr, nullptr, parent) {}
  CompanionFilePresence load(std::span<uint8_t> output) {
    const auto address = reinterpret_cast<uintptr_t>(output.data());
    const auto internal = reinterpret_cast<uintptr_t>(scratch.data());
    const auto distance = address > internal ? address - internal : internal - address;
    if (output.size() != scratch.size() || distance < scratch.size() || !validPath(path)) {
      failure("load arguments");
      return CompanionFilePresence::Error;
    }
    const auto presence = lookup.inspect(path);
    if (presence == CompanionFilePresence::Missing) return presence;
    HalFile file;
    LegacyTintaBackupManifestView view;
    if (presence != CompanionFilePresence::Present || !Storage.openFileForRead("COMPANION", path, file) ||
        file.isDirectory() || file.fileSize64() != scratch.size() ||
        file.read(scratch.data(), scratch.size()) != static_cast<int>(scratch.size()) ||
        file.fileSize64() != scratch.size() || !view.decode(scratch)) {
      failure("load lookup/read/format");
      return CompanionFilePresence::Error;
    }
    std::copy(scratch.begin(), scratch.end(), output.begin());
    return CompanionFilePresence::Present;
  }
  bool persist(std::span<const uint8_t> expected) {
    LegacyTintaBackupManifestView view;
    const auto address = reinterpret_cast<uintptr_t>(expected.data());
    const auto internal = reinterpret_cast<uintptr_t>(scratch.data());
    const auto distance = address > internal ? address - internal : internal - address;
    if (!validPath(path) || !validPath(stagePath) || samePath(path, stagePath) || distance < scratch.size() ||
        !view.decode(expected))
      return failure("arguments/overlap");
    const auto current = inspect(path, expected);
    if (current == Presence::Same) return true;
    if (current != Presence::Missing) return failure("immutable destination");
    const auto pending = inspect(stagePath, expected);
    if (pending == Presence::Missing) {
      if (!stage.begin(stagePath, expected.size()) || !stage.write(0, expected) || !stage.seal(expected.size())) {
        stage.abort();
        return failure("stage write/seal");
      }
    } else if (pending != Presence::Same) {
      return failure("stage conflict");
    } else if (!stage.resume(stagePath, expected.size(), expected.size()) || !stage.seal(expected.size())) {
      return failure("matching stage sync/seal");
    }
    if (inspect(stagePath, expected) != Presence::Same || inspect(path, expected) != Presence::Missing)
      return failure("publication recheck");
    if (!Storage.rename(stagePath, path)) return failure("publication rename");
    return inspect(path, expected) == Presence::Same || failure("publication readback");
  }

 private:
  friend class HalLegacyTintaBackupCapture;
  bool discardOwnedPartial(std::span<const uint8_t> expected) {
    LegacyTintaBackupManifestView view;
    if (!view.decode(expected) || !validPath(path) || !validPath(stagePath) || samePath(path, stagePath))
      return failure("owned stage arguments");
    const auto destination = lookup.inspect(path);
    if (destination == CompanionFilePresence::Present) return true;
    if (destination != CompanionFilePresence::Missing) return failure("owned stage destination lookup");
    const auto pending = lookup.inspect(stagePath);
    if (pending == CompanionFilePresence::Missing) return true;
    if (pending != CompanionFilePresence::Present) return failure("owned stage lookup");
    HalFile file;
    if (!Storage.openFileForRead("COMPANION", stagePath, file) || file.isDirectory())
      return failure("owned stage open");
    const auto extent = file.fileSize64();
    if (extent >= expected.size()) return true;
    const auto count = static_cast<size_t>(extent);
    if ((count && file.read(scratch.data(), count) != static_cast<int>(count)) || file.fileSize64() != extent ||
        !std::equal(scratch.begin(), scratch.begin() + count, expected.begin()))
      return failure("owned stage prefix");
    if (!file.close()) return failure("owned stage close");
    return Storage.remove(stagePath) || failure("owned stage remove");
  }
  static bool validPath(const char* path) {
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
  enum class Presence { Missing, Same, Other, Error };
  Presence inspect(const char* target, std::span<const uint8_t> expected) {
    const auto presence = lookup.inspect(target);
    if (presence == CompanionFilePresence::Missing) return Presence::Missing;
    if (presence == CompanionFilePresence::Error) return Presence::Error;
    HalFile file;
    if (!Storage.openFileForRead("COMPANION", target, file) || file.isDirectory() ||
        file.fileSize64() != scratch.size() ||
        file.read(scratch.data(), scratch.size()) != static_cast<int>(scratch.size()) ||
        file.fileSize64() != scratch.size())
      return Presence::Error;
    LegacyTintaBackupManifestView view;
    if (!view.decode(scratch)) return Presence::Other;
    return std::equal(scratch.begin(), scratch.end(), expected.begin(), expected.end()) ? Presence::Same
                                                                                        : Presence::Other;
  }
  static bool samePath(const char* a, const char* b) {
    const auto lower = [](unsigned char value) { return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value; };
    while (*a && *b && lower(*a) == lower(*b)) {
      ++a;
      ++b;
    }
    return *a == 0 && *b == 0;
  }
  static bool failure(const char* operation) {
    LOG_ERR("COMPANION", "Legacy backup manifest publication failed: %s", operation);
    return false;
  }
  const char* path;
  const char* stagePath;
  std::array<uint8_t, LEGACY_TINTA_BACKUP_MANIFEST_SIZE> scratch{};
  HalCompanionFileLookup lookup;
  HalVerifiedFileStage stage;
};
}  // namespace companion
