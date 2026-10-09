#include "HalTransferStorage.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstring>

#include "CompanionCourseBinding.h"
#include "CompanionCourseSwitchPublication.h"
#include "CompanionDictionaryInstallationPlan.h"
#include "CompanionDictionaryJournalPaths.h"
#include "CompanionFirmwareTransfer.h"
#include "HalCompanionDictionaryInstaller.h"
#include "HalCompanionFontInstallation.h"
#include "HalCourseStateMigration.h"
#include "HalDictionaryCacheStorage.h"
#include "HalDictionaryFinalizedContentVerification.h"
#include "HalInventoryFileHash.h"
#if LILA_TINTA
#include "CompanionCourseStatePaths.h"
#include "CompanionStoredCourseContinuity.h"
#include "CompanionTintaJournalPaths.h"
#include "HalCompanionHeapAdmission.h"
#include "HalCourseValidation.h"
#include "HalRemovedCourseBaseline.h"
#endif

namespace companion {
std::unique_ptr<HalDictionaryTransferInstaller> createHalDictionaryTransferInstaller(const Transfer& transfer,
                                                                                     const Identity& generation,
                                                                                     std::span<uint8_t> scratch) {
  auto installer = makeUniqueNoThrow<HalCompanionDictionaryInstaller>(transfer, generation, scratch);
  if (!installer) LOG_ERR("COMPANION", "OOM: dictionary transfer installer");
  return installer;
}
namespace {
bool failure(const char* operation, const char* path) {
  LOG_ERR("COMPANION", "%s failed: %s", operation, path);
  return false;
}
bool dictionaryContext(const char* destination, const ContentManifest& manifest, const TransferState& state) {
  return destination && validDictionaryInstallationBase(std::string_view(destination, strnlen(destination, 128))) &&
         validDictionaryBindingManifest(manifest) && matchesTransferManifest(manifest, state) &&
         inventory_detail::nonzero(state.owner) && inventory_detail::nonzero(state.transaction) &&
         inventory_detail::nonzero(state.storageGeneration);
}
bool verifyRetiredDictionary(const char* destination, const ContentManifest& manifest, const TransferState& state,
                             std::span<uint8_t> scratch) {
  if (!dictionaryContext(destination, manifest, state) || state.phase != TransferPhase::Committed ||
      state.durableOffset != state.length)
    return failure("retired dictionary context", destination ? destination : "");
  // Discovery buffers and retained HAL handles exceed the task-local budget.
  auto verifier = makeUniqueNoThrow<HalDictionaryFinalizedContentVerification>(scratch);
  if (!verifier) return failure("OOM: retired dictionary verification", destination);
  return verifier->verifyRetired(destination, manifest, state);
}
}  // namespace

bool HalTransferStorage::installDictionaryMembers(const char* destination, const ContentManifest& manifest,
                                                  const TransferState& state, std::span<uint8_t> workspace) {
  return (dictionaryInstaller && dictionaryContext(destination, manifest, state) &&
          state.phase == TransferPhase::Installing && state.durableOffset == state.length &&
          dictionaryInstaller->install(destination, manifest, state, workspace)) ||
         failure("dictionary member installation", destination ? destination : "");
}

bool HalTransferStorage::verifyDictionaryArchive(const char* destination, const ContentManifest& manifest,
                                                 const TransferState& state, std::span<uint8_t> workspace) {
  if (!destination || !validDictionaryInstallationBase(std::string_view(destination, strnlen(destination, 128))) ||
      !validDictionaryBindingManifest(manifest) || !matchesTransferManifest(manifest, state) ||
      state.durableOffset != state.length ||
      (state.phase != TransferPhase::Installing && state.phase != TransferPhase::Committed))
    return failure("dictionary archive arguments", destination ? destination : "");
  uint64_t length = 0;
  const auto stage = stat(TRANSFER_STAGE, length);
  if (stage == FileStatus::Error) return failure("dictionary archive stage lookup", TRANSFER_STAGE);
  if (stage == FileStatus::Present) return verify(TRANSFER_STAGE, manifest.length, manifest.contentHash, workspace);
  struct CacheVerification {
    HalDictionaryCacheStorage storage{[](void*) {
      vTaskDelay(1);
      return true;
    }};
    DictionaryCachePublication publication;
    explicit CacheVerification(std::span<uint8_t> scratch) : publication(storage, scratch) {}
  };
  auto verification = makeUniqueNoThrow<CacheVerification>(workspace);
  if (!verification) return failure("OOM: dictionary archive verifier", destination);
  return verification->publication.find(manifest) == DictionaryCacheResult::Ok ||
         failure("retained dictionary archive verification", destination);
}

bool HalTransferStorage::prepare() {
  if (!Storage.ready()) return failure("SD unavailable", TRANSFER_DIRECTORY);
  return Storage.ensureDirectoryExists(TRANSFER_DIRECTORY) || failure("mkdir", TRANSFER_DIRECTORY);
}

FileStatus HalTransferStorage::stat(const char* path, uint64_t& size) {
  if (!Storage.ready()) return FileStatus::Error;
  bool recoverySlot = false;
  if (path) {
    for (const auto paths : {DICTIONARY_EXTRACTION_JOURNALS, DICTIONARY_INSTALLATION_JOURNALS,
                             DICTIONARY_RETIREMENT_JOURNALS, DICTIONARY_ZIP_AUDIT_JOURNALS})
      for (unsigned slot = 0; slot < 2; ++slot) recoverySlot |= std::strcmp(path, paths[slot]) == 0;
  }
  if (recoverySlot) {
    const auto presence = companionLookup.inspect(path);
    if (presence == CompanionFilePresence::Error) return FileStatus::Error;
    if (presence == CompanionFilePresence::Missing) return FileStatus::Missing;
  } else if (!Storage.exists(path)) {
    return FileStatus::Missing;
  }
  HalFile file;
  if (!Storage.openFileForRead("COMPANION", path, file) || file.isDirectory()) return FileStatus::Error;
  size = file.fileSize64();
  return FileStatus::Present;
}

bool HalTransferStorage::read(const char* path, uint64_t offset, std::span<uint8_t> bytes) {
  HalFile file;
  if (!Storage.openFileForRead("COMPANION", path, file) || !file.seek64(offset) ||
      file.read(bytes.data(), bytes.size()) != static_cast<int>(bytes.size()))
    return failure("read", path);
  return true;
}

bool HalTransferStorage::write(const char* path, uint64_t offset, std::span<const uint8_t> bytes, bool truncate) {
  auto file = Storage.open(path, O_WRONLY | O_CREAT | (truncate ? O_TRUNC : 0));
  if (!file || !file.seek64(offset) || file.write(bytes.data(), bytes.size()) != bytes.size() ||
      !file.truncate(offset + bytes.size()) || !file.sync())
    return failure("write/sync", path);
  return true;
}

bool HalTransferStorage::resize(const char* path, uint64_t size) {
  auto file = Storage.open(path, O_WRONLY);
  if (!file || !file.truncate(size) || !file.sync()) return failure("truncate/sync", path);
  return true;
}

bool HalTransferStorage::rename(const char* from, const char* to) {
  return Storage.rename(from, to) || failure("rename", from);
}
bool HalTransferStorage::remove(const char* path) { return Storage.remove(path) || failure("remove", path); }

#if LILA_TINTA
bool HalTransferStorage::verifyTerminalCourseSwitch(const CourseSwitchRequest& request, const ContentManifest& manifest,
                                                    TransferPhase phase, std::span<uint8_t> workspace) {
  ContentManifest binding;
  bool present = false;
  uint64_t size = 0;
  if (stat(COURSE_BINDING_STAGE, size) != FileStatus::Missing ||
      stat(COURSE_BINDING_BACKUP, size) != FileStatus::Missing ||
      readCourseBinding(*this, COURSE_BINDING_PATH, workspace, binding, present) != CourseBindingResult::Ok || !present)
    return false;
  if (phase == TransferPhase::Committed
          ? binding != manifest
          : binding.logicalIdentity != request.previousCourse || binding.contentHash != request.previousHash)
    return false;
  return verify(ACTIVE_COURSE_PATH, binding.length, binding.contentHash, workspace);
}
bool HalTransferStorage::prepareCourseSwitch(const CourseSwitchRequest& request, const ContentManifest& manifest,
                                             std::span<uint8_t> workspace) {
  ContentManifest previous;
  Identity selected{};
  bool present = false, stateBound = false;
  if (readCourseBinding(*this, COURSE_BINDING_PATH, workspace, previous, present) != CourseBindingResult::Ok ||
      !present || previous.logicalIdentity != request.previousCourse || previous.contentHash != request.previousHash ||
      !verify(ACTIVE_COURSE_PATH, previous.length, previous.contentHash, workspace) ||
      !selectActiveCourseState(*this, workspace, selected, stateBound) || !stateBound ||
      selected != previous.logicalIdentity) {
    LOG_ERR("COMPANION", "Course switch previous state unavailable");
    return false;
  }
  char directory[COURSE_STATE_DIRECTORY_SIZE];
  if (!courseStateDirectory(manifest.logicalIdentity, directory) || !Storage.ensureDirectoryExists(directory)) {
    LOG_ERR("COMPANION", "Cannot prepare switched course state");
    return false;
  }
  return true;
}
bool HalTransferStorage::validateCourse(const char* path, const ContentManifest& manifest, std::span<uint8_t> workspace,
                                        char* locale) {
  if (!courseValidator) {
    // Reuse the ~750-byte parser across commit/recovery; it exceeds the stack budget.
    courseValidator = makeUniqueNoThrow<tinta::core::pack::Pack>();
    if (!courseValidator) {
      LOG_ERR("COMPANION", "OOM: course validator");
      return false;
    }
  }
  CourseCandidateDetails details;
  if (!validateStagedCourse(path, *courseValidator, workspace, details)) return false;
  if (manifest.formatVersion != details.major) {
    LOG_ERR("COMPANION", "Course format does not match manifest");
    return false;
  }
  if (locale) std::memcpy(locale, details.locale, sizeof(details.locale));
  return true;
}
#endif

bool HalTransferStorage::finalizeContentMetadata([[maybe_unused]] const char* destination,
                                                 [[maybe_unused]] const ContentManifest& manifest,
                                                 [[maybe_unused]] const TransferState& state,
                                                 [[maybe_unused]] std::span<uint8_t> workspace) {
  if (manifest.kind == ContentKind::Dictionary) {
    if (state.phase == TransferPhase::Aborted && !dictionaryInstaller) return true;
    if (!dictionaryInstaller && state.phase == TransferPhase::Committed)
      return verifyRetiredDictionary(destination, manifest, state, workspace);
    return (dictionaryInstaller && dictionaryContext(destination, manifest, state) &&
            (state.phase == TransferPhase::Committed || state.phase == TransferPhase::Aborted) &&
            dictionaryInstaller->finalize(destination, manifest, state, workspace)) ||
           failure("dictionary finalization", destination ? destination : "");
  }
#if LILA_TINTA
  if (manifest.kind == ContentKind::Course && std::strcmp(destination, ACTIVE_COURSE_PATH) == 0) {
    CourseSwitchIntent intent(*this, workspace);
    CourseSwitchRequest request;
    const auto result = intent.load(request);
    uint64_t size = 0;
    if (stat(COURSE_SWITCH_INTENT_STAGE, size) != FileStatus::Missing) {
      LOG_ERR("COMPANION", "Pending course switch consent stage");
      return false;
    }
    if (result == CourseSwitchIntentResult::Missing) {
      if (state.phase == TransferPhase::Aborted) return true;
      // Proof banks and checked file handles exceed the task-local stack budget.
      if (!admitCompanionHeap(sizeof(HalRemovedCourseBaseline), sizeof(HalRemovedCourseBaseline)))
        return failure("course retirement heap admission", destination);
      auto retirement = makeUniqueNoThrow<HalRemovedCourseBaseline>(
          state.storageGeneration, workspace, [](void*) { return admitCompanionHeap(); }, nullptr);
      if (!retirement) return failure("OOM: course removal retirement", destination);
      return retirement->retireInstalled(manifest, state);
    }
    if (result != CourseSwitchIntentResult::Ok || request.transaction != state.transaction ||
        request.generation != state.storageGeneration || request.nextHash != manifest.contentHash ||
        request.nextCourse != manifest.logicalIdentity || !matchesTransferManifest(manifest, state) ||
        (state.phase != TransferPhase::Committed && state.phase != TransferPhase::Aborted) ||
        !verifyTerminalCourseSwitch(request, manifest, state.phase, workspace)) {
      LOG_ERR("COMPANION", "Cannot retire course switch consent");
      return false;
    }
    return remove(COURSE_SWITCH_INTENT_PATH);
  }
#endif
  return true;
}

bool HalTransferStorage::validateContent(const char* destination, const char* candidate,
                                         const ContentManifest& manifest, [[maybe_unused]] const TransferState& state,
                                         std::span<uint8_t> workspace) {
  if (manifest.kind == ContentKind::Dictionary)
    return (dictionaryInstaller && dictionaryContext(destination, manifest, state) &&
            (state.phase == TransferPhase::Receiving || state.phase == TransferPhase::Verified) &&
            state.durableOffset == state.length &&
            dictionaryInstaller->prepare(destination, candidate, manifest, state, workspace)) ||
           failure("dictionary preparation", destination ? destination : "");
#if LILA_TINTA
  if (manifest.kind == ContentKind::Course && std::strcmp(destination, ACTIVE_COURSE_PATH) == 0) {
    CourseSwitchIntent intent(*this, workspace);
    CourseSwitchRequest request;
    const auto result = intent.load(request);
    if (result != CourseSwitchIntentResult::Missing) {
      if (result != CourseSwitchIntentResult::Ok || !matchesCourseSwitchTransfer(request, manifest, state) ||
          state.phase != TransferPhase::Receiving || !validateCourse(candidate, manifest, workspace)) {
        LOG_ERR("COMPANION", "Course switch consent or candidate invalid");
        return false;
      }
      return prepareCourseSwitch(request, manifest, workspace);
    }
    const bool retainedContext = matchesTransferManifest(manifest, state) && inventory_detail::nonzero(state.owner) &&
                                 inventory_detail::nonzero(state.transaction) &&
                                 inventory_detail::nonzero(state.storageGeneration) &&
                                 state.durableOffset == state.length &&
                                 (state.phase == TransferPhase::Receiving || state.phase == TransferPhase::Verified);
    return validateCourseContent(candidate, manifest, workspace, retainedContext ? &state.storageGeneration : nullptr);
  }
#endif
  return validateContent(destination, candidate, manifest, workspace);
}

bool HalTransferStorage::installContentMetadata(const char* destination, const ContentManifest& manifest,
                                                [[maybe_unused]] const TransferState& state,
                                                std::span<uint8_t> workspace) {
  if (manifest.kind == ContentKind::Dictionary) {
    if (!dictionaryInstaller && state.phase == TransferPhase::Committed)
      return verifyRetiredDictionary(destination, manifest, state, workspace);
    return (dictionaryInstaller && dictionaryContext(destination, manifest, state) &&
            (state.phase == TransferPhase::Installing || state.phase == TransferPhase::Committed) &&
            state.durableOffset == state.length &&
            dictionaryInstaller->metadata(destination, manifest, state, workspace)) ||
           failure("dictionary metadata", destination ? destination : "");
  }
#if LILA_TINTA
  if (manifest.kind == ContentKind::Course && std::strcmp(destination, ACTIVE_COURSE_PATH) == 0) {
    CourseSwitchIntent intent(*this, workspace);
    CourseSwitchRequest request;
    const auto result = intent.load(request);
    if (result != CourseSwitchIntentResult::Missing) {
      if (result != CourseSwitchIntentResult::Ok || !matchesCourseSwitchTransfer(request, manifest, state) ||
          (state.phase != TransferPhase::Installing && state.phase != TransferPhase::Committed) ||
          !validateCourse(destination, manifest, workspace) ||
          publishSwitchedCourseBinding(*this, request, manifest, workspace) != CourseSwitchIntentResult::Ok) {
        LOG_ERR("COMPANION", "Course switch binding publication failed");
        return false;
      }
      return true;
    }
  }
#endif
  return installContentMetadata(destination, manifest, workspace);
}

bool HalTransferStorage::validateContent([[maybe_unused]] const char* destination,
                                         [[maybe_unused]] const char* candidate, const ContentManifest& manifest,
                                         [[maybe_unused]] std::span<uint8_t> workspace) {
  if (manifest.kind == ContentKind::Font)
    return validateCompanionFontInstallation(destination, candidate, manifest, workspace, true);
  if (manifest.kind == ContentKind::Epub) return true;
  if (manifest.kind == ContentKind::Firmware) {
    if (std::strcmp(destination, FIRMWARE_STAGE_DESTINATION) != 0 || !validFirmwareStageManifest(manifest) ||
        !firmwareValidator || !firmwareValidator(candidate)) {
      LOG_ERR("COMPANION", "Firmware staging validation failed");
      return false;
    }
    return Storage.ensureDirectoryExists("/Companion") || failure("mkdir", "/Companion");
  }
#if LILA_TINTA
  if (manifest.kind == ContentKind::Course && std::strcmp(destination, ACTIVE_COURSE_PATH) == 0) {
    return validateCourseContent(candidate, manifest, workspace);
  }
#endif
  LOG_ERR("COMPANION", "Unsupported content installation");
  return false;
}

#if LILA_TINTA
bool HalTransferStorage::validateCourseContent(const char* candidate, const ContentManifest& manifest,
                                               std::span<uint8_t> workspace, const Identity* generation) {
  char candidateLocale[9];
  if (!validateCourse(candidate, manifest, workspace, candidateLocale)) return false;
  bool hasLearnerState = false;
  static constexpr const char* LEARNER_FILES[] = {
      "/tinta/items.bin",    "/tinta/items.bin.tmp",   "/tinta/reviews.log", "/tinta/reviews.log.tmp",
      "/tinta/profile.bin",  "/tinta/profile.bin.tmp", "/tinta/days.bin",    "/tinta/days.bin.tmp",
      "/tinta/session.bin",  "/tinta/session.bin.tmp", "/tinta/starred.bin", "/tinta/starred.bin.tmp",
      "/tinta/read.bin",     "/tinta/read.bin.tmp",    TINTA_JOURNAL_EVENTS, TINTA_JOURNAL_HEADER_A,
      TINTA_JOURNAL_HEADER_B};
  for (const auto* path : LEARNER_FILES) {
    uint64_t size = 0;
    const auto status = stat(path, size);
    if (status == FileStatus::Error) return failure("learner state stat", path);
    hasLearnerState |= status == FileStatus::Present;
    if (std::strncmp(path, "/tinta/", 7) == 0) {
      char scoped[COURSE_STATE_PATH_SIZE];
      if (!courseStatePath(manifest.logicalIdentity, path + 7, scoped)) return failure("course state path", path);
      const auto scopedStatus = stat(scoped, size);
      if (scopedStatus == FileStatus::Error) return failure("course state stat", scoped);
      hasLearnerState |= scopedStatus == FileStatus::Present;
    }
  }
  const auto result = authorizeCourseReplacement(*this, manifest, hasLearnerState, workspace);
  std::unique_ptr<HalRemovedCourseBaseline> removed;
  const char* previousPath = ACTIVE_COURSE_PATH;
  if (result == CourseBindingResult::HashMismatch && generation) {
    // Retained proof buffers and HAL handles exceed the task-local stack budget.
    if (!admitCompanionHeap(sizeof(HalRemovedCourseBaseline), sizeof(HalRemovedCourseBaseline)))
      return failure("removed course heap admission", candidate);
    removed = makeUniqueNoThrow<HalRemovedCourseBaseline>(
        *generation, workspace, [](void*) { return admitCompanionHeap(); }, nullptr);
    if (!removed) return failure("OOM: removed course baseline", candidate);
    if (!removed->open(manifest) || !(previousPath = removed->path())) return false;
  } else if (result != CourseBindingResult::Ok) {
    LOG_ERR("COMPANION", "Course replacement refused: %u", static_cast<unsigned>(result));
    return false;
  }
  uint64_t currentSize = 0;
  const auto currentStatus = stat(previousPath, currentSize);
  if (currentStatus == FileStatus::Error) return failure("active course stat", ACTIVE_COURSE_PATH);
  if (removed && currentStatus != FileStatus::Present) return failure("removed course stat", previousPath);
  if (currentStatus == FileStatus::Present) {
    char currentLocale[9];
    const auto* previousManifest = removed ? removed->manifest() : &manifest;
    if (!previousManifest || !validateCourse(previousPath, *previousManifest, workspace, currentLocale)) return false;
    for (size_t at = 0; at < sizeof(candidateLocale); ++at) {
      const auto lower = [](unsigned char byte) { return byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte; };
      if (lower(candidateLocale[at]) != lower(currentLocale[at])) {
        LOG_ERR("COMPANION", "Course replacement language differs");
        return false;
      }
    }
    const auto yield = [] { vTaskDelay(1); };
    const auto continuity = compareStoredCourseItemIdentities(*this, previousPath, candidate, yield, workspace);
    const bool legacy = continuity == CourseItemContinuity::MissingHistory;
    if (continuity != CourseItemContinuity::Compatible &&
        !(legacy && (!hasLearnerState || sameLegacyCourseRecords(*this, previousPath, candidate, workspace, yield)))) {
      LOG_ERR("COMPANION", "Course item continuity refused: %u", static_cast<unsigned>(continuity));
      return false;
    }
  }
  if (removed && (!removed->path() || !removed->closeReaders())) return failure("removed course close", previousPath);
  return Storage.ensureDirectoryExists("/tinta") || failure("mkdir", "/tinta");
}
#endif

bool HalTransferStorage::installContentMetadata([[maybe_unused]] const char* destination,
                                                const ContentManifest& manifest,
                                                [[maybe_unused]] std::span<uint8_t> workspace) {
  if (manifest.kind == ContentKind::Font)
    return validateCompanionFontInstallation(destination, destination, manifest, workspace, false);
  if (manifest.kind == ContentKind::Epub) return true;
  if (manifest.kind == ContentKind::Firmware) {
    if (std::strcmp(destination, FIRMWARE_STAGE_DESTINATION) == 0 && validFirmwareStageManifest(manifest) &&
        firmwareValidator && firmwareValidator(destination))
      return true;
    LOG_ERR("COMPANION", "Staged firmware recovery validation failed");
    return false;
  }
#if LILA_TINTA
  if (manifest.kind == ContentKind::Course && std::strcmp(destination, ACTIVE_COURSE_PATH) == 0) {
    if (!validateCourse(destination, manifest, workspace)) return false;
    const auto result = installCourseBinding(*this, manifest, workspace);
    if (result == CourseBindingResult::Ok) return true;
    LOG_ERR("COMPANION", "Course binding recovery failed: %u", static_cast<unsigned>(result));
    return false;
  }
#endif
  LOG_ERR("COMPANION", "Unsupported content metadata");
  return false;
}

bool HalTransferStorage::verify(const char* path, uint64_t length, const Digest& hash, std::span<uint8_t> workspace) {
  if (workspace.empty()) return failure("missing hash workspace", path);
  HalFile file;
  if (!Storage.openFileForRead("COMPANION", path, file) || file.fileSize64() != length)
    return failure("verify length", path);
  Digest actual{};
  uint64_t actualLength = 0;
  if (!hashInventoryFile(file, workspace, actualLength, actual) || actualLength != length || actual != hash)
    return failure("verify SHA-256", path);
  return true;
}

}  // namespace companion
