#pragma once

#include <Memory.h>

#include "CompanionCourseStatePaths.h"
#include "CompanionLegacyTintaBackupPaths.h"
#include "CompanionTintaJournal.h"
#include "CompanionTintaMigrationAdmission.h"
#include "HalLegacyTintaBackupCapture.h"

namespace companion {
// Allocate with makeUniqueNoThrow; caller retains source paths and excludes writers through every operation.
class HalLegacyTintaBackupSession {
 public:
  bool captureCourse(const Identity& reader, const Identity& generation, const Identity& course,
                     const Identity& transaction, bool bound) {
    if (exportReady || !legacyTintaBackupDirectory(reader, generation, course, transaction, directory) ||
        !legacyTintaBackupMetadataPath(reader, generation, course, transaction, LegacyTintaBackupMetadata::Intent,
                                       metadata[2]))
      return failure("course capture state/paths");
    if (!prepareDirectories()) return false;
    const auto presence = destinationLookup.inspect(metadata[2].data());
    if (presence == CompanionFilePresence::Present)
      return prepareCourse(reader, generation, course, transaction, bound, true) && recover();
    if (presence != CompanionFilePresence::Missing) return failure("course capture intent lookup");
    return prepareCourse(reader, generation, course, transaction, bound) && run();
  }
  ~HalLegacyTintaBackupSession() { closeExport(); }
  bool closeExport() {
    exportReady = false;
    return !exportFile.isOpen() || exportFile.close() || failure("export close");
  }
  bool exportComplete() const { return !exportReady && exportOffset == exportLength; }
  bool readManifest(std::span<uint8_t> output) {
    if (output.size() != LEGACY_TINTA_BACKUP_MANIFEST_SIZE) return failure("manifest output size");
    return verify() && capture->loadPublished(output);
  }
  // Caller freezes native writers. This proof is distinct from backup recovery,
  // which can succeed after the original files have disappeared.
  bool matchesCurrentCourse() {
    if (!courseSourcesReady || !closeExport() || !readManifest(verifiedManifest))
      return failure("current course manifest");
    LegacyTintaBackupManifestView view;
    if (!view.decode(verifiedManifest)) return failure("current course manifest decode");
    for (size_t at = 0; at < LEGACY_TINTA_BACKUP_ROLES; ++at) {
      const auto role = static_cast<LegacyTintaBackupRole>(at);
      if (!NAMES[at]) {
        if (view.present(role)) return failure("unsupported current course role");
        continue;
      }
      const auto count =
          snprintf(sourcePaths[at].data(), sourcePaths[at].size(), "%s/%s", sourceDirectory.data(), NAMES[at]);
      if (count <= 0 || static_cast<size_t>(count) >= sourcePaths[at].size()) return failure("current source path");
      const auto presence = sourceLookup.inspect(sourcePaths[at].data());
      if (presence == CompanionFilePresence::Error ||
          (presence == CompanionFilePresence::Present) != view.present(role))
        return failure("current source presence");
      if (!view.present(role)) continue;
      if (!Storage.openFileForReadReusing("COMPANION", sourcePaths[at].data(), exportFile))
        return failure("current source open");
      uint64_t length = 0;
      Digest hash{};
      const bool verified = !exportFile.isDirectory() && hashInventoryFile(exportFile, scratch, length, hash) &&
                            length == view.length(role) &&
                            std::equal(hash.begin(), hash.end(), view.hash(role).begin());
      const bool closed = exportFile.close();
      if (!verified || !closed) return failure("current source hash/close");
    }
    return true;
  }
  // Backup proof only: caller also authenticates owner, validates installed pack
  // and journal retention, and freezes writers through admission publication.
  bool verifyMigrationBackup(const TintaMigrationAdmission& admission, TintaJournalStorage& hashing) {
    if (!validTintaMigrationAdmission(admission) || admission.reader != reader ||
        admission.merge.generation != generation || admission.course != course ||
        admission.backupTransaction != transaction)
      return failure("migration backup identities");
    if (!matchesCurrentCourse()) return failure("migration current sources");
    Digest digest{};
    return (hashing.digest(verifiedManifest, digest) && digest == admission.backupManifest) ||
           failure("migration backup manifest digest");
  }
  bool openExport(LegacyTintaBackupRole role, uint64_t offset = 0) {
    if (!closeExport() || static_cast<size_t>(role) >= LEGACY_TINTA_BACKUP_ROLES ||
        !readManifest(std::span<uint8_t>(scratch).first(LEGACY_TINTA_BACKUP_MANIFEST_SIZE)))
      return failure("export binding");
    LegacyTintaBackupManifestView view;
    if (!view.decode(std::span<const uint8_t>(scratch).first(LEGACY_TINTA_BACKUP_MANIFEST_SIZE)) || !view.present(role))
      return failure("export role");
    exportLength = view.length(role);
    if (offset > exportLength) return failure("export resume bounds");
    std::copy(view.hash(role).begin(), view.hash(role).end(), exportHash.begin());
    if (!Storage.openFileForReadReusing("COMPANION", backups[static_cast<size_t>(role)], exportFile) ||
        !exportMatches()) {
      closeExport();
      return failure("export file");
    }
    exportOffset = offset;
    exportReady = true;
    return true;
  }
  bool readExport(uint64_t offset, std::span<uint8_t> output, size_t& written) {
    written = 0;
    if (!exportReady || offset != exportOffset || output.empty() || output.size() > 768 ||
        exportFile.fileSize64() != exportLength)
      return failure("export read bounds");
    const auto count = static_cast<size_t>(std::min<uint64_t>(output.size(), exportLength - offset));
    if (!exportFile.seek64(offset) || (count && exportFile.read(output.data(), count) != static_cast<int>(count)) ||
        exportFile.fileSize64() != exportLength) {
      closeExport();
      return failure("export read");
    }
    if (offset + count == exportLength && (!exportMatches() || !closeExport())) {
      closeExport();
      return failure("export final verification");
    }
    exportOffset += count;
    written = count;
    return true;
  }
  bool prepareCourse(const Identity& reader, const Identity& generation, const Identity& course,
                     const Identity& transaction, bool bound, bool recovering = false) {
    capture.reset();
    courseSourcesReady = false;
    if (!closeExport()) return false;
    if (!tinta_body_detail::nonzero(reader) || !tinta_body_detail::nonzero(generation) ||
        !tinta_body_detail::nonzero(course) || !tinta_body_detail::nonzero(transaction))
      return failure("course identities");
    if (bound) {
      if (!courseStateDirectory(course, sourceDirectory)) return failure("course directory");
    } else {
      std::copy_n("/tinta", 7, sourceDirectory.begin());
    }
    LegacyTintaBackupManifestView intent;
    if (recovering) {
      if (!legacyTintaBackupDirectory(reader, generation, course, transaction, directory) ||
          !legacyTintaBackupMetadataPath(reader, generation, course, transaction, LegacyTintaBackupMetadata::Intent,
                                         metadata[2]))
        return failure("course recovery paths");
      {
        auto loader =
            makeUniqueNoThrow<HalLegacyTintaBackupManifestStore>(directory.data(), metadata[2].data(), nullptr);
        if (!loader) return failure("OOM: course intent loader");
        if (loader->load(std::span<uint8_t>(scratch).first(LEGACY_TINTA_BACKUP_MANIFEST_SIZE)) !=
            CompanionFilePresence::Present)
          return failure("course recovery intent");
      }
      if (!intent.decode(std::span<const uint8_t>(scratch).first(LEGACY_TINTA_BACKUP_MANIFEST_SIZE)) ||
          intent.present(LegacyTintaBackupRole::Lessons) || intent.present(LegacyTintaBackupRole::Usage) ||
          !std::equal(intent.reader().begin(), intent.reader().end(), reader.begin()) ||
          !std::equal(intent.generation().begin(), intent.generation().end(), generation.begin()) ||
          !std::equal(intent.course().begin(), intent.course().end(), course.begin()) ||
          !std::equal(intent.transaction().begin(), intent.transaction().end(), transaction.begin()))
        return failure("course recovery binding/roles");
    }
    sources = {};
    for (size_t at = 0; at < LEGACY_TINTA_BACKUP_ROLES; ++at) {
      if (!NAMES[at]) continue;
      if (recovering && !intent.present(static_cast<LegacyTintaBackupRole>(at))) continue;
      const auto count =
          snprintf(sourcePaths[at].data(), sourcePaths[at].size(), "%s/%s", sourceDirectory.data(), NAMES[at]);
      if (count <= 0 || static_cast<size_t>(count) >= sourcePaths[at].size()) return failure("course source path");
      if (recovering) {
        sources[at] = sourcePaths[at].data();
        continue;
      }
      const auto presence = sourceLookup.inspect(sourcePaths[at].data());
      if (presence == CompanionFilePresence::Error || (at < 3 && presence != CompanionFilePresence::Present))
        return failure("course source lookup");
      if (presence == CompanionFilePresence::Present) sources[at] = sourcePaths[at].data();
    }
    courseSourcesReady = prepare(reader, generation, course, transaction, sources);
    return courseSourcesReady;
  }
  bool prepare(const Identity& reader, const Identity& generation, const Identity& course, const Identity& transaction,
               std::span<const char* const> sources) {
    courseSourcesReady = false;
    if (!closeExport()) return false;
    capture.reset();
    if (sources.size() != LEGACY_TINTA_BACKUP_ROLES ||
        !legacyTintaBackupDirectory(reader, generation, course, transaction, directory))
      return failure("identities/sources");
    this->reader = reader;
    this->generation = generation;
    this->course = course;
    this->transaction = transaction;
    paths = {};
    backups = {};
    for (size_t at = 0; at < sources.size(); ++at) {
      if (!sources[at]) {
        if (at < 3) return failure("mandatory source");
        continue;
      }
      const auto role = static_cast<LegacyTintaBackupRole>(at);
      if (!legacyTintaBackupFilePath(reader, generation, course, transaction, role, false, backupPaths[at]) ||
          !legacyTintaBackupFilePath(reader, generation, course, transaction, role, true, candidatePaths[at]))
        return failure("file paths");
      paths[at] = {sources[at], candidatePaths[at].data(), backupPaths[at].data()};
      backups[at] = backupPaths[at].data();
    }
    for (size_t at = 0; at < metadata.size(); ++at)
      if (!legacyTintaBackupMetadataPath(reader, generation, course, transaction,
                                         static_cast<LegacyTintaBackupMetadata>(at), metadata[at]))
        return failure("metadata paths");
    capture = makeUniqueNoThrow<HalLegacyTintaBackupCapture>(directory.data(), metadata[0].data(), metadata[1].data(),
                                                             scratch, nullptr, nullptr, metadata[2].data(),
                                                             metadata[3].data());
    return capture != nullptr || failure("OOM: capture");
  }
  bool run() {
    if (exportReady) return failure("export active");
    if (!capture) return failure("unprepared capture");
    if (!prepareDirectories()) return false;
    return capture->captureSources(reader, generation, course, transaction, paths);
  }
  bool recover() {
    if (exportReady) return failure("export active");
    return capture ? capture->recoverOwned(reader, generation, course, transaction, paths)
                   : failure("unprepared recovery");
  }
  bool verify() {
    return capture ? capture->verifySaved(reader, generation, course, transaction, backups)
                   : failure("unprepared verification");
  }

 private:
  static constexpr const char* NAMES[] = {"reviews.log", "items.bin", "profile.bin", nullptr,      "read.bin",
                                          "starred.bin", nullptr,     "days.bin",    "session.bin"};
  bool prepareDirectories() {
    for (size_t at = 1; directory[at]; ++at) {
      if (directory[at] != '/') continue;
      directory[at] = '\0';
      const bool ready = Storage.ensureDirectoryExists(directory.data());
      directory[at] = '/';
      if (!ready) return failure("parent directory");
    }
    if (!Storage.ensureDirectoryExists(directory.data())) return failure("transaction directory");
    return true;
  }
  bool exportMatches() {
    uint64_t length = 0;
    Digest hash{};
    return exportFile.isOpen() && !exportFile.isDirectory() && exportFile.fileSize64() == exportLength &&
           hashInventoryFile(exportFile, scratch, length, hash) && length == exportLength && hash == exportHash;
  }
  static bool failure(const char* operation) {
    LOG_ERR("COMPANION", "Legacy backup session failed: %s", operation);
    return false;
  }
  Identity reader{}, generation{}, course{}, transaction{};
  std::array<char, COURSE_STATE_DIRECTORY_SIZE> sourceDirectory{};
  std::array<std::array<char, COURSE_STATE_PATH_SIZE>, LEGACY_TINTA_BACKUP_ROLES> sourcePaths{};
  std::array<const char*, LEGACY_TINTA_BACKUP_ROLES> sources{};
  HalCompanionFileLookup sourceLookup{nullptr, nullptr, sourceDirectory.data()};
  std::array<char, LEGACY_TINTA_BACKUP_DIRECTORY_SIZE> directory{};
  HalCompanionFileLookup destinationLookup{nullptr, nullptr, directory.data()};
  std::array<std::array<char, LEGACY_TINTA_BACKUP_PATH_SIZE>, LEGACY_TINTA_BACKUP_ROLES> backupPaths{},
      candidatePaths{};
  std::array<std::array<char, LEGACY_TINTA_BACKUP_PATH_SIZE>, 4> metadata{};
  std::array<LegacyTintaBackupPaths, LEGACY_TINTA_BACKUP_ROLES> paths{};
  std::array<const char*, LEGACY_TINTA_BACKUP_ROLES> backups{};
  std::array<uint8_t, 512> scratch{};
  std::array<uint8_t, LEGACY_TINTA_BACKUP_MANIFEST_SIZE> verifiedManifest{};
  std::unique_ptr<HalLegacyTintaBackupCapture> capture;
  HalFile exportFile;
  Digest exportHash{};
  uint64_t exportLength = 0, exportOffset = 0;
  bool exportReady = false;
  bool courseSourcesReady = false;
};
}  // namespace companion
