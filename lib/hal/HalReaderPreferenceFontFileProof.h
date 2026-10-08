#pragma once

#include "CompanionBitmapFontValidation.h"
#include "CompanionInventoryPaths.h"
#include "CompanionVectorFontValidation.h"
#include "HalInventoryFileHash.h"
#include "HalInventoryFileView.h"

namespace companion {
// Retain off-stack; scratch is borrowed from the serialized application phase.
class HalReaderPreferenceFontFileProof final {
 public:
  explicit HalReaderPreferenceFontFileProof(std::span<uint8_t> scratch)
      : scratch(scratch), bitmap(view, scratch), vector(view, scratch) {}
  ~HalReaderPreferenceFontFileProof() { close(); }
  bool verify(std::string_view source, std::span<const char> family, uint8_t pointSize,
              std::span<const uint8_t> expectedHash, bool vectorAllowed) {
    ready = false;
    if (!close() || scratch.size() < 32 || !pointSize || !validInventoryPath(source) || family.empty() ||
        family.size() > 31 || (!expectedHash.empty() && expectedHash.size() != 32))
      return failure("arguments");
    const auto root = source.starts_with("/.fonts/") ? size_t{8} : source.starts_with("/fonts/") ? size_t{7} : 0;
    if (!root) return failure("font root");
    const auto relative = source.substr(root);
    const auto separator = relative.find('/');
    const bool raster = relative.ends_with(".cpfont");
    const std::string_view name(family.data(), family.size());
    if (separator != std::string_view::npos) {
      if (relative.substr(0, separator) != name || relative.find('/', separator + 1) != std::string_view::npos)
        return failure("family path");
    } else if (raster || relative.size() < 5 || relative.substr(0, relative.size() - 4) != name)
      return failure("loose family path");
    const auto filename = separator == std::string_view::npos ? relative : relative.substr(separator + 1);
    if (raster) {
      const auto suffix = filename.size() - 7;
      const auto underscore = filename.substr(0, suffix).find_last_of('_');
      if (underscore == std::string_view::npos || underscore == 0 || underscore + 1 == suffix)
        return failure("point size suffix");
      unsigned size = 0;
      for (const auto digit : filename.substr(underscore + 1, suffix - underscore - 1)) {
        if (digit < '0' || digit > '9' || size > 255) return failure("point size digits");
        size = size * 10 + static_cast<unsigned>(digit - '0');
      }
      if (size != pointSize) return failure("point size mismatch");
    } else {
      if (!vectorAllowed || filename.size() < 5) return failure("vector support");
      const auto suffix = filename.substr(filename.size() - 4);
      bool extension = false;
      for (const auto known : {std::string_view(".ttf"), std::string_view(".otf"), std::string_view(".ttc")}) {
        bool same = true;
        for (size_t at = 0; at < 4; ++at) {
          const char byte = suffix[at];
          if ((byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte) != known[at]) same = false;
        }
        extension |= same;
      }
      if (!extension) return failure("vector extension");
    }
    std::copy(source.begin(), source.end(), path.begin());
    path[source.size()] = 0;
    if (!Storage.openFileForReadReusing("COMPANION", path.data(), file) || !view.attach(file)) {
      close();
      return failure("open/view");
    }
    BitmapFontDetails rasterDetails;
    VectorFontDetails vectorDetails;
    bool valid = raster ? bitmap.validate(rasterDetails) : vector.validate(vectorDetails);
    if (valid) valid = hashInventoryFile(file, scratch, length, digest);
    if (valid && !expectedHash.empty()) valid = std::equal(expectedHash.begin(), expectedHash.end(), digest.begin());
    const bool closed = close();
    if (!valid || !closed) return failure("validation/hash/close");
    ready = true;
    return true;
  }
  const Digest* contentHash() const { return ready ? &digest : nullptr; }

 private:
  bool close() {
    if (!file.isOpen() || file.close()) return true;
    return failure("close");
  }
  static bool failure(const char* reason) {
    LOG_ERR("COMPANION", "Reader font preference proof failed: %s", reason);
    return false;
  }
  std::span<uint8_t> scratch;
  HalFile file;
  HalInventoryFileView view;
  BitmapFontValidation bitmap;
  VectorFontValidation vector;
  std::array<char, INVENTORY_PATH_LIMIT + 1> path{};
  Digest digest{};
  uint64_t length = 0;
  bool ready = false;
};
}  // namespace companion
