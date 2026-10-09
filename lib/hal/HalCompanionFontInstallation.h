#pragma once

#include <Memory.h>

#include <cstring>

#include "../EpdFont/VectorFontSupport.h"
#include "CompanionBitmapFontValidation.h"
#include "CompanionFontDestination.h"
#include "CompanionTransfer.h"
#include "CompanionVectorFontValidation.h"
#include "HalInventoryFileView.h"

namespace companion {
// Retained handles, validators, and destination directory exceed the task-local budget.
class HalCompanionFontInstallation {
 public:
  explicit HalCompanionFontInstallation(std::span<uint8_t> scratch) : bitmap(view, scratch), vector(view, scratch) {}
  bool validate(const char* destination, const char* candidate, const ContentManifest& manifest) {
    if (!destination || !candidate || manifest.kind != ContentKind::Font || manifest.logicalIdentity != Identity{} ||
        !manifest.length || !directory(destination, manifest.formatVersion))
      return failure("destination or manifest");
    if (!Storage.openFileForRead("COMPANION", candidate, file) || file.fileSize64() != manifest.length ||
        !view.attach(file))
      return failure("candidate");
    if (manifest.formatVersion == 4) {
      BitmapFontDetails details;
      if (!bitmap.validate(details)) return failure("bitmap content");
    } else {
#if CROSSPOINT_VECTOR_FONTS
      VectorFontDetails details;
      if (!vector.validate(details)) return failure("vector content");
#else
      return failure("vector fonts require PSRAM");
#endif
    }
    return true;
  }
  bool prepareDirectory() { return Storage.ensureDirectoryExists(parent) || failure("directory creation"); }

 private:
  bool directory(const char* path, uint32_t format) {
    if (!validFontDestination(path, format)) return false;
    const char* name = std::strrchr(path, '/') + 1;
    const size_t parentLength = name - path - 1;
    std::memcpy(parent, path, parentLength);
    parent[parentLength] = 0;
    return true;
  }
  static bool failure(const char* reason) {
    LOG_ERR("COMPANION", "Font installation failed: %s", reason);
    return false;
  }
  HalFile file;
  HalInventoryFileView view;
  BitmapFontValidation bitmap;
  VectorFontValidation vector;
  char parent[TRANSFER_TARGET_SIZE]{};
};
inline bool validateCompanionFontInstallation(const char* destination, const char* candidate,
                                              const ContentManifest& manifest, std::span<uint8_t> scratch,
                                              bool prepareDirectory) {
  auto workspace = makeUniqueNoThrow<HalCompanionFontInstallation>(scratch);
  if (!workspace) {
    LOG_ERR("COMPANION", "OOM: font installation validation workspace");
    return false;
  }
  return workspace->validate(destination, candidate, manifest) && (!prepareDirectory || workspace->prepareDirectory());
}
}  // namespace companion
