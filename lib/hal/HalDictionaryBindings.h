#pragma once

#include <HalStorage.h>
#include <Logging.h>
#include <mbedtls/sha256.h>

#include "CompanionDictionaryArchiveBinding.h"
#include "CompanionDictionaryCachePublication.h"
#include "CompanionInventoryPaths.h"
#include "HalCompanionFileLookup.h"

namespace companion {
// Session-owned; scratch is borrowed and at least DICTIONARY_BINDING_SIZE.
// One record handle and two directory-lookup handles are retained. The parent
// owns recovery and prevents concurrent member/cache/binding mutations.
class HalDictionaryBindings final : public DictionaryArchiveBindings {
 public:
  HalDictionaryBindings(DictionaryCacheStorage& cache, std::span<uint8_t> scratch,
                        HalCompanionFileLookup::Progress progress = nullptr, void* context = nullptr)
      : publication(cache, scratch), scratch(scratch), lookup(progress, context) {
    mbedtls_sha256_init(&hashContext);
  }
  ~HalDictionaryBindings() override {
    close();
    mbedtls_sha256_free(&hashContext);
  }
  HalDictionaryBindings(const HalDictionaryBindings&) = delete;
  HalDictionaryBindings& operator=(const HalDictionaryBindings&) = delete;
  DictionaryBindingResult read(const char* basePath, DictionaryArchiveBinding& output) override {
    if (!paths(basePath)) return DictionaryBindingResult::Error;
    const auto stagedPresence = lookup.inspect(stage.data());
    const auto backupPresence = lookup.inspect(backup.data());
    if (stagedPresence != CompanionFilePresence::Missing || backupPresence != CompanionFilePresence::Missing) {
      failure("pending installation");
      return DictionaryBindingResult::Error;
    }
    const auto result = readFile(target.data(), active);
    if (result == DictionaryBindingResult::Found) output = active;
    return result;
  }
  // Verify the final binding and both retained archives without cleanup writes.
  bool verifyFinalized(const char* basePath, const DictionaryArchiveBinding& binding) {
    if (read(basePath, active) != DictionaryBindingResult::Found || active != binding || !verifyArchives(binding))
      return failure("finalized verification");
    return true;
  }
  // Call only under a durable Installing parent with this exact binding,
  // after original ZIP/member semantic validation. The backup remains owned
  // by the parent until its durable commit and finalizeInstallation().
  bool install(const char* basePath, const DictionaryArchiveBinding& binding) {
    if (!paths(basePath) || !verifyArchives(binding)) return failure("installation arguments or archives");
    const auto current = readFile(target.data(), active);
    if (current == DictionaryBindingResult::Error) return false;
    if (current == DictionaryBindingResult::Found && active == binding) return true;
    const auto saved = readFile(backup.data(), previous);
    if (saved == DictionaryBindingResult::Error) return false;
    if (current == DictionaryBindingResult::Found && saved == DictionaryBindingResult::Found)
      return failure("conflicting active and backup bindings");
    auto bytes = scratch.first(DICTIONARY_BINDING_SIZE);
    if (encodeDictionaryBinding(pathHash, binding, bytes) != DICTIONARY_BINDING_SIZE ||
        !Storage.ensureDirectoryExists(TRANSFER_DIRECTORY) ||
        !Storage.openFileForWriteReusing("COMPANION", stage.data(), file))
      return failure("stage open");
    const bool written =
        file.write(bytes.data(), bytes.size()) == bytes.size() && file.truncate(bytes.size()) && file.sync();
    const bool closed = close();
    if (!written || !closed) return failure("stage write/sync");
    if (readFile(stage.data(), staged) != DictionaryBindingResult::Found || staged != binding)
      return failure("stage readback");
    if (current == DictionaryBindingResult::Found && !Storage.rename(target.data(), backup.data()))
      return failure("backup rename");
    if (!Storage.rename(stage.data(), target.data())) return failure("publication rename");
    if (readFile(target.data(), active) != DictionaryBindingResult::Found || active != binding)
      return failure("published readback");
    return true;
  }
  // Call only after the parent is durably Committed and retains recovery data.
  // Normal reads remain closed to a partially cleaned transaction.
  bool finalizeInstallation(const char* basePath, const DictionaryArchiveBinding& binding) {
    if (!paths(basePath) || !verifyArchives(binding) ||
        readFile(target.data(), active) != DictionaryBindingResult::Found || active != binding)
      return failure("finalization binding or archives");
    // A durably committed parent no longer needs its owned backup for rollback.
    if (!remove(stage.data()) || !remove(backup.data())) return false;
    return true;
  }

 private:
  DictionaryCachePublication publication;
  std::span<uint8_t> scratch;
  HalFile file;
  HalCompanionFileLookup lookup;
  mbedtls_sha256_context hashContext;
  Digest pathHash{};
  std::array<char, 120> target{}, stage{}, backup{};
  DictionaryArchiveBinding active, previous, staged;
  bool paths(const char* basePath) {
    if (!close() || !Storage.ready() || scratch.size() < DICTIONARY_BINDING_SIZE || !basePath)
      return failure("path arguments");
    const auto size = strnlen(basePath, 512);
    if (size == 512 || !validInventoryPath(std::string_view(basePath, size))) return failure("unsafe base path");
    if (mbedtls_sha256_starts(&hashContext, 0) != 0 ||
        mbedtls_sha256_update(&hashContext, reinterpret_cast<const uint8_t*>(basePath), size) != 0 ||
        mbedtls_sha256_finish(&hashContext, pathHash.data()) != 0)
      return failure("path SHA");
    static constexpr char PREFIX[] = "/.crosspoint/companion/dictionary-binding-";
    static constexpr char HEX_DIGITS[] = "0123456789abcdef";
    static_assert(sizeof(PREFIX) + 64 + 4 <= 120);
    std::copy_n(PREFIX, sizeof(PREFIX) - 1, target.begin());
    size_t at = sizeof(PREFIX) - 1;
    for (const auto byte : pathHash) {
      target[at++] = HEX_DIGITS[byte >> 4];
      target[at++] = HEX_DIGITS[byte & 15];
    }
    target[at] = 0;
    std::copy_n(target.begin(), at, stage.begin());
    std::copy_n(target.begin(), at, backup.begin());
    std::copy_n(".tmp", 5, stage.begin() + at);
    std::copy_n(".bak", 5, backup.begin() + at);
    return true;
  }
  bool verifyArchives(const DictionaryArchiveBinding& binding) {
    return validDictionaryBindingManifest(binding.members) && validDictionaryBindingManifest(binding.original) &&
           publication.find(binding.members) == DictionaryCacheResult::Ok &&
           publication.find(binding.original) == DictionaryCacheResult::Ok;
  }
  DictionaryBindingResult readFile(const char* path, DictionaryArchiveBinding& output) {
    if (!close() || !Storage.ready()) return error("read close or SD unavailable");
    const auto presence = lookup.inspect(path);
    if (presence == CompanionFilePresence::Error) return error("presence lookup");
    if (presence == CompanionFilePresence::Missing) return DictionaryBindingResult::Missing;
    if (!Storage.openFileForReadReusing("COMPANION", path, file)) return error("read open");
    const bool regular = !file.isDirectory();
    const bool sized = regular && file.fileSize64() == DICTIONARY_BINDING_SIZE;
    auto bytes = scratch.first(DICTIONARY_BINDING_SIZE);
    const bool read = sized && file.read(bytes.data(), bytes.size()) == static_cast<int>(bytes.size());
    const bool closed = close();
    if (!read || !closed || !decodeDictionaryBinding(pathHash, bytes, output)) return error("record readback");
    return DictionaryBindingResult::Found;
  }
  bool remove(const char* path) {
    if (!close()) return false;
    const auto presence = lookup.inspect(path);
    if (presence == CompanionFilePresence::Error) return failure("cleanup lookup");
    return presence == CompanionFilePresence::Missing || Storage.remove(path) || failure("cleanup");
  }
  bool close() {
    if (file.isOpen() && !file.close()) return failure("close");
    return true;
  }
  bool failure(const char* operation) {
    LOG_ERR("COMPANION", "Dictionary binding %s failed", operation);
    return false;
  }
  DictionaryBindingResult error(const char* operation) {
    failure(operation);
    return DictionaryBindingResult::Error;
  }
};
}  // namespace companion
