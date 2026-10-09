#pragma once

#include "CompanionTransfer.h"
#include "HalCompanionFileLookup.h"
#include "HalDictionaryTransferInstaller.h"
#if LILA_TINTA
#include <memory>

#include "core/pack/Pack.h"
#endif

namespace companion {
struct CourseSwitchRequest;

class HalTransferStorage final : public TransferStorage {
 public:
  // Set by firmware entry points; nullptr keeps firmware staging unavailable.
  // Callback validates only and must never flash. It borrows the file path for the call.
  void setFirmwareValidator(bool (*validator)(const char*)) { firmwareValidator = validator; }
  void setDictionaryInstaller(HalDictionaryTransferInstaller* installer) { dictionaryInstaller = installer; }
  bool installDictionaryMembers(const char*, const ContentManifest&, const TransferState&, std::span<uint8_t>) override;
  bool prepare() override;
  FileStatus stat(const char* path, uint64_t& size) override;
  bool read(const char* path, uint64_t offset, std::span<uint8_t> bytes) override;
  bool write(const char* path, uint64_t offset, std::span<const uint8_t> bytes, bool truncate) override;
  bool resize(const char* path, uint64_t size) override;
  bool rename(const char* from, const char* to) override;
  bool remove(const char* path) override;
  bool validateContent(const char* destination, const char* candidate, const ContentManifest& manifest,
                       std::span<uint8_t> workspace) override;
  bool installContentMetadata(const char* destination, const ContentManifest& manifest,
                              std::span<uint8_t> workspace) override;
  bool validateContent(const char* destination, const char* candidate, const ContentManifest& manifest,
                       const TransferState& state, std::span<uint8_t> workspace) override;
  bool installContentMetadata(const char* destination, const ContentManifest& manifest, const TransferState& state,
                              std::span<uint8_t> workspace) override;
  bool finalizeContentMetadata(const char* destination, const ContentManifest& manifest, const TransferState& state,
                               std::span<uint8_t> workspace) override;
  bool verifyCourseSwitchSource(const ContentManifest& previous, const Identity& generation,
                                std::span<uint8_t> workspace) override;
  bool verify(const char* path, uint64_t length, const Digest& hash, std::span<uint8_t> workspace) override;
  bool verifyDictionaryArchive(const char* destination, const ContentManifest& manifest, const TransferState& state,
                               std::span<uint8_t> workspace) override;

 private:
  // Reuse two lookup handles; admission requires checked absence of recovery slots.
  HalCompanionFileLookup companionLookup;
  bool (*firmwareValidator)(const char*) = nullptr;
  HalDictionaryTransferInstaller* dictionaryInstaller = nullptr;
#if LILA_TINTA
  bool inspectCourseSwitchSource(const ContentManifest& previous, const Identity& generation,
                                 std::span<uint8_t> workspace, bool& removed);
  bool verifyTerminalCourseSwitch(const CourseSwitchRequest& request, const ContentManifest& manifest,
                                  TransferPhase phase, std::span<uint8_t> workspace);
  bool prepareCourseSwitch(const CourseSwitchRequest& request, const ContentManifest& manifest,
                           std::span<uint8_t> workspace);
  bool validateCourse(const char* path, const ContentManifest& manifest, std::span<uint8_t> workspace,
                      char* locale = nullptr);
  bool validateCourseContent(const char* candidate, const ContentManifest& manifest, std::span<uint8_t> workspace,
                             const Identity* generation = nullptr);
  std::unique_ptr<tinta::core::pack::Pack> courseValidator;
#endif
};

}  // namespace companion
