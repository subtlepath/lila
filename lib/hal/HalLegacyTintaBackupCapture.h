#pragma once

#include "HalLegacyTintaBackupCopy.h"
#include "HalLegacyTintaBackupManifestStore.h"

namespace companion {
struct LegacyTintaBackupPaths {
  const char* original = nullptr;
  const char* candidate = nullptr;
  const char* backup = nullptr;
};
// Checked heap workspace. Caller retains paths/scratch, prepares parent and excludes all source/backup writers.
class HalLegacyTintaBackupCapture {
 public:
  HalLegacyTintaBackupCapture(const char* parent, const char* manifestPath, const char* manifestStage,
                              std::span<uint8_t> scratch, InventoryHashProgress progress = nullptr,
                              void* context = nullptr, const char* intentPath = nullptr,
                              const char* intentStage = nullptr)
      : manifestPath(manifestPath),
        manifestStage(manifestStage),
        intentPath(intentPath),
        intentStage(intentStage),
        scratch(scratch),
        progress(progress),
        context(context),
        copier(parent, scratch, progress, context),
        manifestStore(parent, manifestPath, manifestStage),
        intentStore(parent, intentPath, intentStage) {}
  bool recover(const Identity& reader, const Identity& generation, const Identity& course, const Identity& transaction,
               std::span<const LegacyTintaBackupPaths> paths) {
    if (!intentPath || !intentStage || intentStore.load(encodedManifest) != CompanionFilePresence::Present)
      return failure("recovery intent");
    LegacyTintaBackupManifestView view;
    if (!view.decode(encodedManifest) || !std::equal(view.reader().begin(), view.reader().end(), reader.begin()) ||
        !std::equal(view.generation().begin(), view.generation().end(), generation.begin()) ||
        !std::equal(view.course().begin(), view.course().end(), course.begin()) ||
        !std::equal(view.transaction().begin(), view.transaction().end(), transaction.begin()))
      return failure("recovery identity binding");
    return capture(encodedManifest, paths);
  }
  bool verifySaved(const Identity& reader, const Identity& generation, const Identity& course,
                   const Identity& transaction, std::span<const char* const> backups) {
    if (backups.size() != LEGACY_TINTA_BACKUP_ROLES || scratch.size() < 64 ||
        overlaps(scratch.data(), scratch.size(), backups.data(), backups.size_bytes()) ||
        manifestStore.load(encodedManifest) != CompanionFilePresence::Present)
      return failure("saved manifest");
    LegacyTintaBackupManifestView view;
    if (!view.decode(encodedManifest) || !std::equal(view.reader().begin(), view.reader().end(), reader.begin()) ||
        !std::equal(view.generation().begin(), view.generation().end(), generation.begin()) ||
        !std::equal(view.course().begin(), view.course().end(), course.begin()) ||
        !std::equal(view.transaction().begin(), view.transaction().end(), transaction.begin()))
      return failure("saved identity binding");
    for (size_t at = 0; at < backups.size(); ++at) {
      const auto role = static_cast<LegacyTintaBackupRole>(at);
      if (!view.present(role)) {
        if (backups[at]) return failure("absent backup path");
        continue;
      }
      if (!HalLegacyTintaBackupCopy::validSourcePath(backups[at]) ||
          HalLegacyTintaBackupCopy::samePath(backups[at], manifestPath) ||
          HalLegacyTintaBackupCopy::samePath(backups[at], manifestStage))
        return failure("saved backup path");
      for (size_t previous = 0; previous < at; ++previous)
        if (backups[previous] && HalLegacyTintaBackupCopy::samePath(backups[at], backups[previous]))
          return failure("saved backup alias");
    }
    for (size_t at = 0; at < backups.size(); ++at) {
      const auto role = static_cast<LegacyTintaBackupRole>(at);
      if (!view.present(role)) continue;
      Digest hash{};
      std::copy(view.hash(role).begin(), view.hash(role).end(), hash.begin());
      if (!copier.verify(backups[at], view.length(role), hash)) return failure("saved backup content");
    }
    return true;
  }
  bool captureSources(const Identity& reader, const Identity& generation, const Identity& course,
                      const Identity& transaction, std::span<const LegacyTintaBackupPaths> paths) {
    if (paths.size() != LEGACY_TINTA_BACKUP_ROLES || scratch.size() < 64 ||
        overlaps(scratch.data(), scratch.size(), paths.data(), paths.size_bytes()) ||
        !tinta_body_detail::nonzero(reader) || !tinta_body_detail::nonzero(generation) ||
        !tinta_body_detail::nonzero(course) || !tinta_body_detail::nonzero(transaction))
      return failure("source arguments");
    sourceManifest = {};
    sourceManifest.reader = reader;
    sourceManifest.generation = generation;
    sourceManifest.course = course;
    sourceManifest.transaction = transaction;
    for (size_t at = 0; at < paths.size(); ++at) {
      const auto& path = paths[at];
      if (!path.original) {
        if (at < 3 || path.candidate || path.backup) return failure("missing source role");
        continue;
      }
      if (!HalLegacyTintaBackupCopy::validSourcePath(path.original)) return failure("source path");
      HalFile file;
      auto& receipt = sourceManifest.files[at];
      const uint64_t limit = at == 0 ? 16 * 1024 * 1024 : UINT32_MAX;
      if (!Storage.openFileForRead("COMPANION", path.original, file) || file.isDirectory() ||
          file.fileSize64() > limit ||
          !hashInventoryFile(file, scratch, receipt.length, receipt.hash, progress, context))
        return failure("source hash");
      receipt.present = true;
    }
    if (!encodeLegacyTintaBackupManifest(sourceManifest, encodedManifest)) return failure("source manifest");
    return capture(encodedManifest, paths);
  }
  bool capture(std::span<const uint8_t> manifest, std::span<const LegacyTintaBackupPaths> paths) {
    LegacyTintaBackupManifestView view;
    if (overlaps(scratch.data(), scratch.size(), manifest.data(), manifest.size()) ||
        overlaps(scratch.data(), scratch.size(), paths.data(), paths.size_bytes()) || !view.decode(manifest) ||
        paths.size() != LEGACY_TINTA_BACKUP_ROLES || !HalLegacyTintaBackupCopy::validSourcePath(manifestPath) ||
        !HalLegacyTintaBackupCopy::validSourcePath(manifestStage) ||
        HalLegacyTintaBackupCopy::samePath(manifestPath, manifestStage))
      return failure("manifest/paths");
    if ((intentPath == nullptr) != (intentStage == nullptr)) return failure("intent path pair");
    if (intentPath && (!HalLegacyTintaBackupCopy::validSourcePath(intentPath) ||
                       !HalLegacyTintaBackupCopy::validSourcePath(intentStage) ||
                       HalLegacyTintaBackupCopy::samePath(intentPath, intentStage) ||
                       HalLegacyTintaBackupCopy::samePath(intentPath, manifestPath) ||
                       HalLegacyTintaBackupCopy::samePath(intentPath, manifestStage) ||
                       HalLegacyTintaBackupCopy::samePath(intentStage, manifestPath) ||
                       HalLegacyTintaBackupCopy::samePath(intentStage, manifestStage)))
      return failure("intent aliases");
    for (size_t at = 0; at < paths.size(); ++at) {
      const auto role = static_cast<LegacyTintaBackupRole>(at);
      const auto& file = paths[at];
      if (!view.present(role)) {
        if (file.original || file.candidate || file.backup) return failure("absent role paths");
        continue;
      }
      for (unsigned field = 0; field < 3; ++field) {
        const char* path = select(file, field);
        if (!HalLegacyTintaBackupCopy::validSourcePath(path) ||
            HalLegacyTintaBackupCopy::samePath(path, manifestPath) ||
            HalLegacyTintaBackupCopy::samePath(path, manifestStage) ||
            (intentPath && (HalLegacyTintaBackupCopy::samePath(path, intentPath) ||
                            HalLegacyTintaBackupCopy::samePath(path, intentStage))))
          return failure("file path");
        for (size_t other = 0; other <= at; ++other) {
          if (!view.present(static_cast<LegacyTintaBackupRole>(other))) continue;
          for (unsigned previous = 0; previous < 3; ++previous) {
            if (other == at && previous >= field) break;
            if (HalLegacyTintaBackupCopy::samePath(path, select(paths[other], previous)))
              return failure("aliased file paths");
          }
        }
      }
    }
    if (intentPath && !intentStore.persist(manifest)) return failure("durable intent");
    for (size_t at = 0; at < paths.size(); ++at) {
      const auto role = static_cast<LegacyTintaBackupRole>(at);
      if (!view.present(role)) continue;
      Digest hash{};
      std::copy(view.hash(role).begin(), view.hash(role).end(), hash.begin());
      if (recoveringOwned && !copier.discardOwnedPartial(paths[at].original, paths[at].candidate, paths[at].backup,
                                                         view.length(role), hash))
        return failure("owned partial recovery");
      if (!copier.copy(paths[at].original, paths[at].candidate, paths[at].backup, view.length(role), hash))
        return failure("file capture");
    }
    if (recoveringOwned && !manifestStore.discardOwnedPartial(manifest)) return failure("owned manifest recovery");
    return manifestStore.persist(manifest);
  }

 private:
  friend class HalLegacyTintaBackupSession;
  bool loadPublished(std::span<uint8_t> output) { return manifestStore.load(output) == CompanionFilePresence::Present; }
  bool recoverOwned(const Identity& reader, const Identity& generation, const Identity& course,
                    const Identity& transaction, std::span<const LegacyTintaBackupPaths> paths) {
    recoveringOwned = true;
    const bool result = recover(reader, generation, course, transaction, paths);
    recoveringOwned = false;
    return result;
  }
  static bool overlaps(const void* a, size_t aSize, const void* b, size_t bSize) {
    if (!aSize || !bSize) return false;
    const auto first = reinterpret_cast<uintptr_t>(a);
    const auto second = reinterpret_cast<uintptr_t>(b);
    return first <= second ? second - first < aSize : first - second < bSize;
  }
  static const char* select(const LegacyTintaBackupPaths& paths, unsigned field) {
    return field == 0 ? paths.original : field == 1 ? paths.candidate : paths.backup;
  }
  static bool failure(const char* operation) {
    LOG_ERR("COMPANION", "Legacy backup capture failed: %s", operation);
    return false;
  }
  const char* manifestPath;
  const char* manifestStage;
  const char* intentPath;
  const char* intentStage;
  std::span<uint8_t> scratch;
  InventoryHashProgress progress;
  void* context;
  LegacyTintaBackupManifest sourceManifest{};
  std::array<uint8_t, LEGACY_TINTA_BACKUP_MANIFEST_SIZE> encodedManifest{};
  HalLegacyTintaBackupCopy copier;
  HalLegacyTintaBackupManifestStore manifestStore;
  HalLegacyTintaBackupManifestStore intentStore;
  bool recoveringOwned = false;
};
}  // namespace companion
