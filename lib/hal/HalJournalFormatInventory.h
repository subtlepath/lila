#pragma once

#include <optional>

#include "HalTintaJournalStorage.h"

namespace companion {
// Session-owned lookup buffers. No files or headers are created or recovered.
class HalJournalFormatInventory {
 public:
  // Bits 0..2 represent validated TJH1..TJH3. Output changes only on success.
  bool inspect(uint8_t& versions) {
    uint8_t result = 0;
    for (const auto location :
         {TintaJournalLocation::Active, TintaJournalLocation::MigrationCandidate, TintaJournalLocation::Backup,
          TintaJournalLocation::MergeCandidate, TintaJournalLocation::MergeBackup}) {
      const auto* paths = tintaJournalPaths(location);
      const auto presence = outer.inspect(paths->directory);
      if (presence == CompanionFilePresence::Error) return failure("directory lookup");
      if (presence == CompanionFilePresence::Missing) continue;
      children.emplace(nullptr, nullptr, paths->directory);
      if (children->inspect(paths->events) != CompanionFilePresence::Present) return failure("events missing");
      {
        HalFile file;
        if (!Storage.openFileForRead("COMPANION", paths->events, file) || file.isDirectory())
          return failure("events type/open");
      }
      bool found = false;
      for (uint8_t slot = 0; slot < 2; ++slot) {
        const auto* path = slot == 0 ? paths->headerA : paths->headerB;
        const auto header = children->inspect(path);
        if (header == CompanionFilePresence::Error) return failure("header lookup");
        if (header == CompanionFilePresence::Missing) continue;
        HalFile file;
        if (!Storage.openFileForRead("COMPANION", path, file) || file.isDirectory() ||
            file.fileSize64() != bytes.size() ||
            file.read(bytes.data(), bytes.size()) != static_cast<int>(bytes.size()))
          return failure("header read");
        const auto version = TintaJournal::headerVersion(bytes, slot);
        if (!version) return failure("header format");
        result |= static_cast<uint8_t>(1u << (version - 1));
        found = true;
      }
      if (!found) return failure("headers missing");
      children.reset();
    }
    versions = result;
    return true;
  }

 private:
  static bool failure(const char* reason) {
    LOG_ERR("COMPANION", "Journal format inventory failed: %s", reason);
    return false;
  }
  HalCompanionFileLookup outer;
  std::optional<HalCompanionFileLookup> children;
  std::array<uint8_t, TintaJournal::HEADER_SIZE> bytes{};
};
}  // namespace companion
