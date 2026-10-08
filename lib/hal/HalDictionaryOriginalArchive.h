#pragma once

#include "CompanionDictionaryArchiveBinding.h"
#include "CompanionDictionaryExtractionParent.h"
#include "HalDictionaryArchiveStage.h"

namespace companion {
// Original ZIP must already pass complete archive/member validation and remain
// exclusively owned. Retention adds an immutable blob, not installed members.
class HalDictionaryOriginalArchive final {
 public:
  HalDictionaryOriginalArchive(DictionaryCacheStorage& cache, std::span<uint8_t> scratch,
                               InventoryHashProgress progress = nullptr, void* context = nullptr)
      : scratch(scratch),
        progress(progress),
        context(context),
        stage(scratch, progress, context),
        publication(cache, scratch) {}
  const char* publishedPath() const { return ready ? publication.publishedPath() : nullptr; }
  bool retain(InventoryIndexStorage& source, const ContentManifest& manifest,
              const DictionaryExtractionParent& parent) {
    ready = false;
    const auto receipt = parent.current();
    if (!receipt || receipt->sealed != (receipt->synonyms ? 15 : 7) || !parent.matchesArchive(manifest) ||
        !validDictionaryBindingManifest(manifest) || scratch.size() < 64)
      return fail("arguments or parent");
    const auto existing = publication.find(manifest);
    if (existing == DictionaryCacheResult::Ok) {
      ready = true;
      return true;
    }
    if (existing != DictionaryCacheResult::Missing && existing != DictionaryCacheResult::Corrupt)
      return fail("cache inspection");
    uint64_t bytes = 0;
    if (!source.size(bytes) || bytes != manifest.length || !tick()) return fail("source extent");
    struct Guard {
      HalDictionaryArchiveStage& stage;
      bool sealed = false;
      ~Guard() {
        if (!sealed) stage.abort();
      }
    } guard{stage};
    if (!stage.begin()) return fail("stage begin");
    for (uint64_t at = 0; at < bytes;) {
      const auto count = static_cast<size_t>(std::min<uint64_t>(scratch.size(), bytes - at));
      const auto chunk = scratch.first(count);
      if (!tick() || !parent.matchesArchive(manifest) || !source.read(at, chunk) || !stage.write(at, chunk))
        return fail("copy");
      at += count;
    }
    uint64_t finalBytes = 0;
    if (!source.size(finalBytes) || finalBytes != bytes || !tick() || !parent.matchesArchive(manifest) ||
        !stage.seal(bytes))
      return fail("seal");
    guard.sealed = true;
    if (stage.contentHash() != manifest.contentHash || publication.publish(manifest) != DictionaryCacheResult::Ok)
      return fail("hash or publication");
    ready = true;
    return true;
  }

 private:
  std::span<uint8_t> scratch;
  InventoryHashProgress progress;
  void* context;
  HalDictionaryArchiveStage stage;
  DictionaryCachePublication publication;
  bool ready = false;
  bool tick() const { return !progress || progress(context); }
  static bool fail(const char* operation) {
    LOG_ERR("COMPANION", "Original dictionary archive retention %s failed", operation);
    return false;
  }
};
}  // namespace companion
