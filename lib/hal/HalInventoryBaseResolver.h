#pragma once

#include <cstring>
#include <string_view>

#include "HalInventoryFileSource.h"

namespace companion {
// Wrap with course/font/dictionary resolvers before scanning transferable content.
class HalInventoryBaseResolver final : public InventoryFileResolver {
 public:
  InventoryFileDecision resolve(const char* path, HalFile& file, ContentManifest& metadata) override {
    if (!path || !file) return failure("missing path or file");
    const auto length = strnlen(path, 512);
    if (length == 512 || !validInventoryPath(std::string_view(path, length))) return failure("unsafe path");
    const std::string_view view(path, length);
    static constexpr std::string_view PRIVATE_ROOT = "/.crosspoint";
    if (view.size() >= PRIVATE_ROOT.size() && equalAscii(view.substr(0, PRIVATE_ROOT.size()), PRIVATE_ROOT) &&
        (view.size() == PRIVATE_ROOT.size() || view[PRIVATE_ROOT.size()] == '/'))
      return InventoryFileDecision::Skip;
    if (file.isDirectory()) return InventoryFileDecision::Include;
    if (suffix(view, ".epub")) {
      metadata = {};
      metadata.kind = ContentKind::Epub;
      metadata.formatVersion = 1;
      return InventoryFileDecision::Include;
    }
    static constexpr std::string_view DELEGATED[] = {".pack", ".cpfont", ".ttf",     ".otf", ".ttc",
                                                     ".ifo",  ".dict",   ".dict.dz", ".idx", ".syn"};
    for (const auto extension : DELEGATED)
      if (suffix(view, extension)) return failure("content requires its installed-content resolver");
    return InventoryFileDecision::Skip;
  }

 private:
  static bool suffix(std::string_view path, std::string_view extension) {
    if (path.size() <= extension.size()) return false;
    return equalAscii(path.substr(path.size() - extension.size()), extension);
  }
  static bool equalAscii(std::string_view value, std::string_view lowerCase) {
    if (value.size() != lowerCase.size()) return false;
    for (size_t at = 0; at < value.size(); ++at) {
      const unsigned char byte = value[at];
      const unsigned char lower = byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte;
      if (lower != lowerCase[at]) return false;
    }
    return true;
  }
  static InventoryFileDecision failure(const char* reason) {
    LOG_ERR("COMPANION", "Inventory base resolution failed: %s", reason);
    return InventoryFileDecision::Error;
  }
};
}  // namespace companion
