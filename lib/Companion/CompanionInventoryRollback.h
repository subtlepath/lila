#pragma once
#include "CompanionInventoryPublication.h"
namespace companion {
class InventoryRollback {
 public:
  static constexpr char INTENTS[2][48] = {"/.crosspoint/companion/inventory-rollback-a",
                                          "/.crosspoint/companion/inventory-rollback-b"};
  InventoryRollback(TransferStorage& storage, InventoryPublicationValidator& validator, std::span<uint8_t> scratch)
      : storage(storage), validator(validator), scratch(scratch) {}
  InventoryPublicationResult begin(const Identity& generation, uint64_t failedRevision) {
    if (scratch.size() < 32 || !inventory_detail::nonzero(generation) || failedRevision == 0)
      return InventoryPublicationResult::Invalid;
    if (!storage.prepare()) return InventoryPublicationResult::IoError;
    for (const char* index : {InventoryPublication::INDEX, InventoryPublication::INDEX_OLD}) {
      for (const char* paths : {InventoryPublication::PATHS, InventoryPublication::PATHS_OLD}) {
        uint64_t revision = 0;
        const auto result = validator.pair(index, paths, generation, 0, &revision);
        if (result == InventoryValidation::IoError) return InventoryPublicationResult::IoError;
        if (result != InventoryValidation::Valid || revision == 0 || revision >= failedRevision) continue;
        if (!persist(generation, revision, 1)) return InventoryPublicationResult::IoError;
        return recover(generation);
      }
    }
    return InventoryPublicationResult::Corrupt;
  }
  InventoryPublicationResult beginEmpty(const Identity& generation, uint64_t failedRevision) {
    if (scratch.size() < 32 || !inventory_detail::nonzero(generation) || failedRevision == 0)
      return InventoryPublicationResult::Invalid;
    if (!storage.prepare()) return InventoryPublicationResult::IoError;
    const auto checked = emptyEligible(generation, failedRevision, true);
    if (checked != InventoryPublicationResult::Ok) return checked;
    if (!persist(generation, failedRevision, 2)) return InventoryPublicationResult::IoError;
    return recover(generation);
  }
  bool pending(bool& value) {
    bool present = false;
    for (const char* path : INTENTS) {
      uint64_t size = 0;
      const auto status = storage.stat(path, size);
      if (status == FileStatus::Error) return false;
      if (status == FileStatus::Present) present = true;
    }
    value = present;
    return true;
  }
  InventoryPublicationResult hasValidIntent(const Identity& generation, bool& output) {
    if (scratch.size() < 32 || !inventory_detail::nonzero(generation)) return InventoryPublicationResult::Invalid;
    bool found = false;
    uint64_t revision = 0;
    uint8_t mode = 0;
    for (const char* path : INTENTS) {
      uint64_t size = 0;
      const auto status = storage.stat(path, size);
      if (status == FileStatus::Error) return InventoryPublicationResult::IoError;
      if (status == FileStatus::Missing || size != 32) continue;
      auto bytes = scratch.first(32);
      if (!storage.read(path, 0, bytes)) return InventoryPublicationResult::IoError;
      Identity stored;
      std::copy_n(bytes.begin() + 4, stored.size(), stored.begin());
      const uint64_t candidate = inventory_detail::read(bytes, 20, 8);
      if (!valid(bytes, stored, candidate)) continue;
      if (stored != generation) return InventoryPublicationResult::WrongStorage;
      if (found && (revision != candidate || mode != bytes[3])) return InventoryPublicationResult::Corrupt;
      found = true;
      revision = candidate;
      mode = bytes[3];
    }
    output = found;
    return InventoryPublicationResult::Ok;
  }
  InventoryPublicationResult recover(const Identity& generation) {
    if (scratch.size() < 32 || !inventory_detail::nonzero(generation)) return InventoryPublicationResult::Invalid;
    if (!storage.prepare()) return InventoryPublicationResult::IoError;
    uint64_t revision = 0;
    uint8_t mode = 0;
    for (const char* intent : INTENTS) {
      uint64_t size = 0;
      const auto status = storage.stat(intent, size);
      if (status == FileStatus::Error) return InventoryPublicationResult::IoError;
      if (status == FileStatus::Missing || size != 32) continue;
      auto bytes = scratch.first(32);
      if (!storage.read(intent, 0, bytes)) return InventoryPublicationResult::IoError;
      Identity stored;
      std::copy_n(bytes.begin() + 4, stored.size(), stored.begin());
      const uint64_t candidate = inventory_detail::read(bytes, 20, 8);
      if (!valid(bytes, stored, candidate)) continue;
      if (stored != generation) return InventoryPublicationResult::WrongStorage;
      if (revision != 0 && (revision != candidate || mode != bytes[3])) return InventoryPublicationResult::Corrupt;
      revision = candidate;
      mode = bytes[3];
    }
    if (revision == 0) return InventoryPublicationResult::Corrupt;
    if (mode == 2) {
      const auto checked = emptyEligible(generation, revision, false);
      if (checked != InventoryPublicationResult::Ok) return checked;
      if (!remove(InventoryPublication::INDEX) || !remove(InventoryPublication::PATHS))
        return InventoryPublicationResult::IoError;
    } else {
      const auto index =
          restore(InventoryPublication::INDEX, InventoryPublication::INDEX_OLD, false, generation, revision);
      if (index != InventoryPublicationResult::Ok) return index;
      const auto paths =
          restore(InventoryPublication::PATHS, InventoryPublication::PATHS_OLD, true, generation, revision);
      if (paths != InventoryPublicationResult::Ok) return paths;
      const auto checked =
          validator.pair(InventoryPublication::INDEX, InventoryPublication::PATHS, generation, revision);
      if (checked != InventoryValidation::Valid) return classify(checked);
    }
    for (const char* path :
         {InventoryPublication::INDEX_NEXT, InventoryPublication::PATHS_NEXT, InventoryPublication::INDEX_OLD,
          InventoryPublication::PATHS_OLD, InventoryPublication::INTENTS[1], InventoryPublication::INTENTS[0],
          INTENTS[1], INTENTS[0]}) {
      if (!remove(path)) return InventoryPublicationResult::IoError;
    }
    return InventoryPublicationResult::Ok;
  }

 private:
  TransferStorage& storage;
  InventoryPublicationValidator& validator;
  std::span<uint8_t> scratch;
  static InventoryPublicationResult classify(InventoryValidation result) {
    return result == InventoryValidation::IoError ? InventoryPublicationResult::IoError
                                                  : InventoryPublicationResult::Corrupt;
  }
  static bool valid(std::span<const uint8_t> bytes, const Identity& generation, uint64_t revision) {
    return bytes[0] == 'R' && bytes[1] == 'B' && bytes[2] == 'U' && (bytes[3] == 1 || bytes[3] == 2) && revision != 0 &&
           std::equal(generation.begin(), generation.end(), bytes.begin() + 4) &&
           inventory_detail::read(bytes, 20, 8) == revision &&
           inventory_detail::read(bytes, 28, 4) == inventoryIndexCrc(bytes.first(28));
  }
  bool persist(const Identity& generation, uint64_t revision, uint8_t mode) {
    for (const char* intent : INTENTS) {
      auto bytes = scratch.first(32);
      bytes[0] = 'R';
      bytes[1] = 'B';
      bytes[2] = 'U';
      bytes[3] = mode;
      std::copy(generation.begin(), generation.end(), bytes.begin() + 4);
      inventory_detail::write(bytes, 20, revision, 8);
      inventory_detail::write(bytes, 28, inventoryIndexCrc(bytes.first(28)), 4);
      if (!storage.write(intent, 0, bytes, true) || !storage.read(intent, 0, bytes) ||
          !valid(bytes, generation, revision) || bytes[3] != mode)
        return false;
    }
    return true;
  }
  InventoryPublicationResult emptyEligible(const Identity& generation, uint64_t revision, bool requirePublication) {
    uint64_t size = 0;
    for (const char* backup : {InventoryPublication::INDEX_OLD, InventoryPublication::PATHS_OLD}) {
      const auto status = storage.stat(backup, size);
      if (status == FileStatus::Error) return InventoryPublicationResult::IoError;
      if (status != FileStatus::Missing) return InventoryPublicationResult::Corrupt;
    }
    if (requirePublication) {
      bool authorized = false;
      for (const char* intent : InventoryPublication::INTENTS) {
        const auto status = storage.stat(intent, size);
        if (status == FileStatus::Error) return InventoryPublicationResult::IoError;
        if (status == FileStatus::Missing || size != 32) continue;
        auto bytes = scratch.first(32);
        if (!storage.read(intent, 0, bytes)) return InventoryPublicationResult::IoError;
        if (bytes[0] != 'I' || bytes[1] != 'P' || bytes[2] != 'U' || bytes[3] != 1 ||
            inventory_detail::read(bytes, 28, 4) != inventoryIndexCrc(bytes.first(28)))
          continue;
        if (!std::equal(generation.begin(), generation.end(), bytes.begin() + 4))
          return InventoryPublicationResult::WrongStorage;
        if (inventory_detail::read(bytes, 20, 8) != revision) return InventoryPublicationResult::Corrupt;
        authorized = true;
      }
      if (!authorized) return InventoryPublicationResult::Corrupt;
    }
    for (bool paths : {false, true}) {
      const char* active = paths ? InventoryPublication::PATHS : InventoryPublication::INDEX;
      const auto status = storage.stat(active, size);
      if (status == FileStatus::Error) return InventoryPublicationResult::IoError;
      if (status == FileStatus::Missing) continue;
      const auto checked = validator.file(active, paths, generation, revision);
      if (checked != InventoryValidation::Valid) return classify(checked);
    }
    return InventoryPublicationResult::Ok;
  }
  bool remove(const char* path) {
    uint64_t size = 0;
    const auto status = storage.stat(path, size);
    return status == FileStatus::Missing || (status == FileStatus::Present && storage.remove(path));
  }
  InventoryPublicationResult restore(const char* active, const char* backup, bool paths, const Identity& generation,
                                     uint64_t revision) {
    auto result = validator.file(active, paths, generation, revision);
    if (result == InventoryValidation::Valid) return InventoryPublicationResult::Ok;
    if (result == InventoryValidation::IoError) return InventoryPublicationResult::IoError;
    result = validator.file(backup, paths, generation, revision);
    if (result != InventoryValidation::Valid) return classify(result);
    if (!remove(active) || !storage.rename(backup, active)) return InventoryPublicationResult::IoError;
    return InventoryPublicationResult::Ok;
  }
};
}  // namespace companion
