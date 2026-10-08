#pragma once

#include <Logging.h>
#include <SdCardFontRegistry.h>

#include "HalReaderPreferenceFontFileProof.h"

namespace companion {
// Registry discovery is complete and frozen for this retained application owner.
class NativeReaderPreferenceFontProof final {
 public:
  NativeReaderPreferenceFontProof(const SdCardFontRegistry& registry, std::span<uint8_t> scratch)
      : registry(registry), proof(scratch) {}
  bool verify(std::span<const char> name, uint8_t pointSize, std::span<const uint8_t> hash) {
    ready = false;
    if (name.empty() || name.size() > 31 || !pointSize) return failure("selection");
    const auto* family = registry.findFamily(std::string_view(name.data(), name.size()));
    if (!family || (family->vector && !CROSSPOINT_VECTOR_FONTS)) return failure("family unavailable");
    const auto* file = family->findFile(family->vector ? 0 : pointSize, 0);
    if (!file) return failure("regular face or size unavailable");
    ready = proof.verify(file->path, name, pointSize, hash, CROSSPOINT_VECTOR_FONTS != 0);
    return ready;
  }
  const Digest* contentHash() const { return ready ? proof.contentHash() : nullptr; }

 private:
  static bool failure(const char* reason) {
    LOG_ERR("COMPANION", "Reader registered font proof failed: %s", reason);
    return false;
  }
  const SdCardFontRegistry& registry;
  HalReaderPreferenceFontFileProof proof;
  bool ready = false;
};
}  // namespace companion
