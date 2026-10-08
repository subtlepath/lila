#pragma once

#include <Memory.h>

#include <cstring>

#include "../EpdFont/VectorFontSupport.h"
#include "CompanionBitmapFontValidation.h"
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
    const char* relative = nullptr;
    if (std::strncmp(path, "/fonts/", 7) == 0) relative = path + 7;
    if (std::strncmp(path, "/.fonts/", 8) == 0) relative = path + 8;
    if (!relative || (format != 4 && format != 1)) return false;
    const size_t length = std::strlen(path);
    if (length >= sizeof(parent)) return false;
    const char* name = std::strrchr(path, '/');
    if (!name || !name[1] || relative[0] == '.' || relative[0] == '_') return false;
    ++name;
    for (const char* at = relative; *at; ++at)
      if (*at == '\\' || static_cast<unsigned char>(*at) < 32) return false;
    const char* slash = std::strchr(relative, '/');
    if (slash && (slash == relative || slash + 1 != name || name[0] == '.' || name[0] == '_')) return false;
    const size_t nameLength = std::strlen(name);
    if (format == 4) {
      if (!slash || nameLength <= 7 || std::strcmp(name + nameLength - 7, ".cpfont")) return false;
      const char* underscore = std::strrchr(name, '_');
      if (!underscore || underscore == name || underscore >= name + nameLength - 7) return false;
      unsigned size = 0;
      for (const char* at = underscore + 1; at != name + nameLength - 7; ++at) {
        if (*at < '0' || *at > '9' || size > 255) return false;
        size = size * 10 + static_cast<unsigned>(*at - '0');
      }
      if (!size || size > 255) return false;
    } else {
      if (nameLength <= 4) return false;
      char suffix[5]{};
      for (unsigned at = 0; at < 4; ++at) {
        const char byte = name[nameLength - 4 + at];
        suffix[at] = byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte;
      }
      if (std::strcmp(suffix, ".ttf") && std::strcmp(suffix, ".otf") && std::strcmp(suffix, ".ttc")) return false;
    }
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
