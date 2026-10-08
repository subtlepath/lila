#pragma once

#include <Memory.h>

#include "CompanionReaderPreferenceReplay.h"
#include "HalDictionaryCacheStorage.h"

namespace companion {
// Short-lived checked heap owner. Scratch is borrowed from boot/sync; registry
// discovery and content/journal/settings writers remain excluded until release.
class NativeReaderPreferenceContentPhase {
 public:
  NativeReaderPreferenceContentPhase(const SdCardFontRegistry& registry, std::span<uint8_t> scratch)
      : bindings(cache, scratch),
        metadata(registry, bindings, scratch, decoded, normalized, wanted, found, nullptr, {}, &borrowDecoder, this) {}
  NativeReaderPreferenceMetadata& contentMetadata() { return metadata; }

 private:
  static bool borrowDecoder(void* context, tinfl_decompressor*& output, std::span<uint8_t>& bytes) {
    auto& owner = *static_cast<NativeReaderPreferenceContentPhase*>(context);
    // Allocate once on first compressed selection, outside member/index loops.
    // Large decompression state cannot use the stack or permanent C3 DRAM.
    if (!owner.decoder) owner.decoder = makeUniqueNoThrow<tinfl_decompressor>();
    if (!owner.decoder) {
      LOG_ERR("COMPANION", "OOM: reader dictionary preference decoder");
      return false;
    }
    if (!owner.window) owner.window = makeUniqueNoThrow<uint8_t[]>(32768);
    if (!owner.window) {
      LOG_ERR("COMPANION", "OOM: reader dictionary preference decoder/window");
      return false;
    }
    output = owner.decoder.get();
    bytes = std::span(owner.window.get(), 32768);
    return true;
  }
  std::array<uint32_t, 256> decoded{};
  std::array<uint32_t, 1024> normalized{};
  std::array<uint8_t, 768> wanted{}, found{};
  std::unique_ptr<tinfl_decompressor> decoder;
  std::unique_ptr<uint8_t[]> window;
  HalDictionaryCacheStorage cache;
  HalDictionaryBindings bindings;
  NativeReaderPreferenceMetadata metadata;
};
class NativeReaderPreferencePhase final : public NativeReaderPreferenceContentPhase {
 public:
  NativeReaderPreferencePhase(CrossPointSettings& settings, const SdCardFontRegistry& registry,
                              std::span<uint8_t> scratch)
      : NativeReaderPreferenceContentPhase(registry, scratch), replay(settings, contentMetadata()) {}
  ReaderPreferenceApplicationResult apply() { return replay.run(); }

 private:
  NativeReaderPreferenceReplay replay;
};
}  // namespace companion
