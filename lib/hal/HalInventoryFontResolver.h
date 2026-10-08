#pragma once

#include <cstring>

#include "CompanionBitmapFontValidation.h"
#include "CompanionVectorFontValidation.h"
#include "HalInventoryFileSource.h"
#include "HalInventoryFileView.h"

namespace companion {
// Adds installed font resolution; other content remains delegated.
class HalInventoryFontResolver final : public InventoryFileResolver {
 public:
  static constexpr uint32_t FORMAT_BITMAP = 4;
  static constexpr uint32_t FORMAT_VECTOR = 1;
  // Scratch borrows the scan's hash bank; resolver state stays session-owned.
  HalInventoryFontResolver(InventoryFileResolver& delegate, std::span<uint8_t> scratch)
      : delegate(delegate), bitmap(view, scratch), vector(view, scratch) {}
  InventoryFileDecision resolve(const char* path, HalFile& file, ContentManifest& metadata) override {
    format = file.isDirectory() ? 0 : fontFormat(path);
    validated = false;
    if (!format) return delegate.resolve(path, file, metadata);
    BitmapFontDetails bitmapDetails;
    VectorFontDetails vectorDetails;
    if (!view.attach(file) || (format == FORMAT_BITMAP && !bitmap.validate(bitmapDetails)) ||
        (format == FORMAT_VECTOR && !vector.validate(vectorDetails)) || !view.size(length)) {
      LOG_ERR("COMPANION", "Installed font validation failed");
      return InventoryFileDecision::Error;
    }
    metadata = {};
    metadata.kind = ContentKind::Font;
    metadata.formatVersion = format;
    validated = true;
    return InventoryFileDecision::Include;
  }
  InventoryFileDecision resolveBundle(const char* path, ContentManifest& metadata, const char*& archivePath) override {
    return delegate.resolveBundle(path, metadata, archivePath);
  }
  bool verifyHashed(const char* path, const ContentManifest& manifest) override {
    if (!format) return delegate.verifyHashed(path, manifest);
    return validated && fontFormat(path) == format && manifest.kind == ContentKind::Font &&
           manifest.formatVersion == format && manifest.logicalIdentity == Identity{} && manifest.length == length;
  }

 private:
  static uint32_t fontFormat(const char* path) {
    if (!path || (std::strncmp(path, "/.fonts/", 8) && std::strncmp(path, "/fonts/", 7))) return 0;
    const auto length = std::strlen(path);
    if (length > 7 && std::strcmp(path + length - 7, ".cpfont") == 0) return FORMAT_BITMAP;
    if (length < 4) return 0;
    const char* suffix = path + length - 4;
    static constexpr char EXTENSIONS[][5] = {".ttf", ".otf", ".ttc"};
    for (const auto& extension : EXTENSIONS) {
      bool matches = true;
      for (unsigned index = 0; index < 4; ++index) {
        const char byte = suffix[index];
        const char lower = byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte;
        if (lower != extension[index]) {
          matches = false;
          break;
        }
      }
      if (matches) return FORMAT_VECTOR;
    }
    return 0;
  }
  InventoryFileResolver& delegate;
  HalInventoryFileView view;
  BitmapFontValidation bitmap;
  VectorFontValidation vector;
  uint64_t length = 0;
  uint32_t format = 0;
  bool validated = false;
};
}  // namespace companion
