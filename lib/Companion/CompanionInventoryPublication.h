#pragma once

#include "CompanionInventoryPaths.h"
#include "CompanionTransfer.h"

namespace companion {
enum class InventoryValidation { Valid, Invalid, IoError };
class InventoryPublicationValidator {
 public:
  virtual ~InventoryPublicationValidator() = default;
  // Revision zero accepts any valid revision; used only before a new intent.
  virtual InventoryValidation file(const char* path, bool paths, const Identity& generation, uint64_t revision) = 0;
  // Validate both complete files and index-to-path correspondence.
  virtual InventoryValidation pair(const char* index, const char* paths, const Identity& generation, uint64_t revision,
                                   uint64_t* actualRevision = nullptr) = 0;
};
enum class InventoryPublicationResult { Ok, Invalid, IoError, Corrupt, WrongStorage };
class InventoryPublication {
 public:
  static constexpr char INDEX[] = "/.crosspoint/companion/inventory";
  static constexpr char INDEX_NEXT[] = "/.crosspoint/companion/inventory-next";
  static constexpr char INDEX_OLD[] = "/.crosspoint/companion/inventory-old";
  static constexpr char PATHS[] = "/.crosspoint/companion/inventory-paths";
  static constexpr char PATHS_NEXT[] = "/.crosspoint/companion/inventory-paths-next";
  static constexpr char PATHS_OLD[] = "/.crosspoint/companion/inventory-paths-old";
  static constexpr char INTENTS[2][48] = {"/.crosspoint/companion/inventory-publish-a",
                                          "/.crosspoint/companion/inventory-publish-b"};
  static constexpr size_t INTENT_SIZE = 32;
  InventoryPublication(TransferStorage& storage, InventoryPublicationValidator& validator, std::span<uint8_t> scratch)
      : storage(storage), validator(validator), scratch(scratch) {}
  InventoryPublicationResult pendingRevision(const Identity& generation, uint64_t& output) {
    if (scratch.size() < INTENT_SIZE || !inventory_detail::nonzero(generation))
      return InventoryPublicationResult::Invalid;
    uint64_t revision = 0;
    bool valid = false;
    const auto result = intent(generation, revision, valid);
    if (result == InventoryPublicationResult::Ok) output = revision;
    return result;
  }
  InventoryPublicationResult recover(const Identity& generation) {
    if (scratch.size() < INTENT_SIZE || !inventory_detail::nonzero(generation))
      return InventoryPublicationResult::Invalid;
    if (!storage.prepare()) return InventoryPublicationResult::IoError;
    uint64_t revision = 0;
    bool haveValidIntent = false;
    const auto result = intent(generation, revision, haveValidIntent);
    if (result == InventoryPublicationResult::Corrupt && haveValidIntent) return result;
    if (result == InventoryPublicationResult::IoError || result == InventoryPublicationResult::WrongStorage)
      return result;
    if (result == InventoryPublicationResult::Corrupt || revision == 0) {
      const auto active = existing(generation);
      if (active != InventoryPublicationResult::Ok) return active;
      if (result == InventoryPublicationResult::Corrupt && !clearIntents()) return InventoryPublicationResult::IoError;
      return InventoryPublicationResult::Ok;
    }
    return install(generation, revision);
  }
  InventoryPublicationResult publish(const Identity& generation, uint64_t revision) {
    if (revision == 0) return InventoryPublicationResult::Invalid;
    const auto recovery = recover(generation);
    if (recovery != InventoryPublicationResult::Ok) return recovery;
    uint64_t size = 0, current = 0;
    const auto active = storage.stat(INDEX, size);
    if (active == FileStatus::Error) return InventoryPublicationResult::IoError;
    if (active == FileStatus::Present) {
      const auto checked = validator.pair(INDEX, PATHS, generation, 0, &current);
      if (checked != InventoryValidation::Valid) return validationResult(checked);
      if (current == 0) return InventoryPublicationResult::Corrupt;
      if (revision <= current) {
        if (revision != current) return InventoryPublicationResult::Invalid;
        const auto indexNext = storage.stat(INDEX_NEXT, size), pathsNext = storage.stat(PATHS_NEXT, size);
        if (indexNext == FileStatus::Error || pathsNext == FileStatus::Error)
          return InventoryPublicationResult::IoError;
        return indexNext == FileStatus::Missing && pathsNext == FileStatus::Missing
                   ? InventoryPublicationResult::Ok
                   : InventoryPublicationResult::Invalid;
      }
    }
    const auto validation = validator.pair(INDEX_NEXT, PATHS_NEXT, generation, revision);
    if (validation != InventoryValidation::Valid) return validationResult(validation);
    for (unsigned slot = 0; slot < 2; ++slot) {
      auto bytes = scratch.first(INTENT_SIZE);
      bytes[0] = 'I';
      bytes[1] = 'P';
      bytes[2] = 'U';
      bytes[3] = 1;
      std::copy(generation.begin(), generation.end(), bytes.begin() + 4);
      inventory_detail::write(bytes, 20, revision, 8);
      inventory_detail::write(bytes, 28, inventoryIndexCrc(bytes.first(28)), 4);
      if (!storage.write(INTENTS[slot], 0, bytes, true) || !storage.read(INTENTS[slot], 0, bytes) ||
          !validIntent(bytes, generation, revision))
        return InventoryPublicationResult::IoError;
    }
    return install(generation, revision);
  }

 private:
  TransferStorage& storage;
  InventoryPublicationValidator& validator;
  std::span<uint8_t> scratch;
  static InventoryPublicationResult validationResult(InventoryValidation value) {
    return value == InventoryValidation::IoError ? InventoryPublicationResult::IoError
                                                 : InventoryPublicationResult::Corrupt;
  }
  static bool validIntent(std::span<const uint8_t> bytes, const Identity& generation, uint64_t revision) {
    return bytes.size() == INTENT_SIZE && bytes[0] == 'I' && bytes[1] == 'P' && bytes[2] == 'U' && bytes[3] == 1 &&
           std::equal(generation.begin(), generation.end(), bytes.begin() + 4) && revision != 0 &&
           inventory_detail::read(bytes, 20, 8) == revision &&
           inventory_detail::read(bytes, 28, 4) == inventoryIndexCrc(bytes.first(28));
  }
  InventoryPublicationResult intent(const Identity& generation, uint64_t& revision, bool& valid) {
    bool present = false;
    valid = false;
    for (unsigned slot = 0; slot < 2; ++slot) {
      uint64_t size = 0;
      const auto status = storage.stat(INTENTS[slot], size);
      if (status == FileStatus::Error) return InventoryPublicationResult::IoError;
      if (status == FileStatus::Missing) continue;
      present = true;
      if (size != INTENT_SIZE) continue;
      auto bytes = scratch.first(INTENT_SIZE);
      if (!storage.read(INTENTS[slot], 0, bytes)) return InventoryPublicationResult::IoError;
      Identity stored;
      std::copy_n(bytes.begin() + 4, stored.size(), stored.begin());
      const uint64_t candidate = inventory_detail::read(bytes, 20, 8);
      if (!validIntent(bytes, stored, candidate)) continue;
      if (stored != generation) return InventoryPublicationResult::WrongStorage;
      if (valid && candidate != revision) return InventoryPublicationResult::Corrupt;
      revision = candidate;
      valid = true;
    }
    return present && !valid ? InventoryPublicationResult::Corrupt : InventoryPublicationResult::Ok;
  }
  bool remove(const char* path) {
    uint64_t size = 0;
    const auto status = storage.stat(path, size);
    return status == FileStatus::Missing || (status == FileStatus::Present && storage.remove(path));
  }
  bool clearIntents() { return remove(INTENTS[1]) && remove(INTENTS[0]); }
  InventoryPublicationResult existing(const Identity& generation) {
    uint64_t size = 0;
    const auto index = storage.stat(INDEX, size), paths = storage.stat(PATHS, size);
    if (index == FileStatus::Error || paths == FileStatus::Error) return InventoryPublicationResult::IoError;
    if (index == FileStatus::Missing && paths == FileStatus::Missing) {
      const auto oldIndex = storage.stat(INDEX_OLD, size), oldPaths = storage.stat(PATHS_OLD, size);
      if (oldIndex == FileStatus::Error || oldPaths == FileStatus::Error) return InventoryPublicationResult::IoError;
      return oldIndex == FileStatus::Missing && oldPaths == FileStatus::Missing ? InventoryPublicationResult::Ok
                                                                                : InventoryPublicationResult::Corrupt;
    }
    if (index != FileStatus::Present || paths != FileStatus::Present) return InventoryPublicationResult::Corrupt;
    const auto validation = validator.pair(INDEX, PATHS, generation, 0);
    if (validation != InventoryValidation::Valid) return validationResult(validation);
    return remove(INDEX_OLD) && remove(PATHS_OLD) ? InventoryPublicationResult::Ok
                                                  : InventoryPublicationResult::IoError;
  }
  InventoryPublicationResult installFile(const char* active, const char* candidate, const char* backup, bool paths,
                                         const Identity& generation, uint64_t revision) {
    uint64_t size = 0;
    const auto status = storage.stat(active, size);
    if (status == FileStatus::Error) return InventoryPublicationResult::IoError;
    if (status == FileStatus::Present) {
      const auto validation = validator.file(active, paths, generation, revision);
      if (validation == InventoryValidation::Valid) return InventoryPublicationResult::Ok;
      if (validation == InventoryValidation::IoError) return InventoryPublicationResult::IoError;
    }
    const auto validation = validator.file(candidate, paths, generation, revision);
    if (validation != InventoryValidation::Valid) return validationResult(validation);
    if (status == FileStatus::Present) {
      const auto old = storage.stat(backup, size);
      if (old == FileStatus::Error) return InventoryPublicationResult::IoError;
      if (old != FileStatus::Missing) return InventoryPublicationResult::Corrupt;
      if (!storage.rename(active, backup)) return InventoryPublicationResult::IoError;
    }
    return storage.rename(candidate, active) ? InventoryPublicationResult::Ok : InventoryPublicationResult::IoError;
  }
  InventoryPublicationResult install(const Identity& generation, uint64_t revision) {
    const auto index = installFile(INDEX, INDEX_NEXT, INDEX_OLD, false, generation, revision);
    if (index != InventoryPublicationResult::Ok) return index;
    const auto paths = installFile(PATHS, PATHS_NEXT, PATHS_OLD, true, generation, revision);
    if (paths != InventoryPublicationResult::Ok) return paths;
    const auto validation = validator.pair(INDEX, PATHS, generation, revision);
    if (validation != InventoryValidation::Valid) return validationResult(validation);
    if (!remove(INDEX_OLD) || !remove(PATHS_OLD) || !remove(INDEX_NEXT) || !remove(PATHS_NEXT) || !clearIntents())
      return InventoryPublicationResult::IoError;
    return InventoryPublicationResult::Ok;
  }
};
}  // namespace companion
