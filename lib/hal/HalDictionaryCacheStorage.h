#pragma once

#include <HalStorage.h>
#include <Logging.h>

#include "CompanionDictionaryCachePublication.h"
#include "HalCompanionFileLookup.h"
#include "HalInventoryFileHash.h"

namespace companion {
// One hash/read handle and two directory-lookup handles are retained and reused.
class HalDictionaryCacheStorage final : public DictionaryCacheStorage {
 public:
  explicit HalDictionaryCacheStorage(InventoryHashProgress progress = nullptr, void* context = nullptr)
      : lookup(progress, context), progress(progress), context(context) {}
  ~HalDictionaryCacheStorage() override { close(); }
  bool prepare() override {
    if (!Storage.ready() || !Storage.ensureDirectoryExists(TRANSFER_DIRECTORY)) return failure("prepare");
    return true;
  }
  DictionaryCacheCheck inspect(const char* path, const ContentManifest& manifest, std::span<uint8_t> scratch) override {
    if (!path || scratch.empty()) return io("invalid inspection arguments");
    if (!close() || !Storage.ready()) return io("close or SD unavailable");
    const auto presence = lookup.inspect(path);
    if (presence == CompanionFilePresence::Error) return io("presence lookup");
    if (presence == CompanionFilePresence::Missing) return DictionaryCacheCheck::Missing;
    if (!Storage.openFileForReadReusing("COMPANION", path, file)) return io("open");
    if (file.isDirectory()) return close() ? DictionaryCacheCheck::Collision : DictionaryCacheCheck::IoError;
    if (file.fileSize64() != manifest.length)
      return close() ? DictionaryCacheCheck::Corrupt : DictionaryCacheCheck::IoError;
    Digest actual{};
    uint64_t length = 0;
    if (!hashInventoryFile(file, scratch, length, actual, progress, context)) {
      close();
      return io("SHA readback");
    }
    if (!close()) return DictionaryCacheCheck::IoError;
    return length == manifest.length && actual == manifest.contentHash ? DictionaryCacheCheck::Valid
                                                                       : DictionaryCacheCheck::Corrupt;
  }
  bool rename(const char* from, const char* to) override {
    if (!close() || !Storage.ready() || !Storage.rename(from, to)) return failure("rename");
    return true;
  }
  bool remove(const char* path) override {
    if (!close() || !Storage.ready()) return failure("remove close or SD unavailable");
    const auto presence = lookup.inspect(path);
    if (presence == CompanionFilePresence::Error) return failure("remove lookup");
    return presence == CompanionFilePresence::Missing || Storage.remove(path) || failure("remove");
  }

 private:
  HalFile file;
  HalCompanionFileLookup lookup;
  InventoryHashProgress progress;
  void* context;
  bool close() {
    if (file.isOpen() && !file.close()) return failure("close");
    return true;
  }
  bool failure(const char* operation) {
    LOG_ERR("COMPANION", "Dictionary cache %s failed", operation);
    return false;
  }
  DictionaryCacheCheck io(const char* operation) {
    failure(operation);
    return DictionaryCacheCheck::IoError;
  }
};
}  // namespace companion
