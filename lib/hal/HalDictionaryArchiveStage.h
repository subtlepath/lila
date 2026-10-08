#pragma once

#include "CompanionDictionaryBundleBuilder.h"
#include "CompanionDictionaryCachePublication.h"
#include "HalVerifiedFileStage.h"

namespace companion {
// Borrowed readback scratch; one serialized cache owner controls publication.
class HalDictionaryArchiveStage final : public DictionaryBundleStage {
 public:
  static constexpr const char* CANDIDATE = DICTIONARY_CACHE_CANDIDATE;
  using Progress = InventoryHashProgress;
  explicit HalDictionaryArchiveStage(std::span<uint8_t> scratch, Progress progress = nullptr, void* context = nullptr)
      : stage(scratch, progress, context) {}
  const Digest& contentHash() const { return stage.contentHash(); }
  bool isSealed() const { return stage.isSealed(); }
  bool begin() override { return stage.begin(CANDIDATE, UINT32_MAX); }
  bool write(uint64_t offset, std::span<const uint8_t> bytes) override { return stage.write(offset, bytes); }
  bool seal(uint64_t bytes) override {
    if (bytes < 22) {
      LOG_ERR("COMPANION", "Dictionary archive stage seal bounds failed");
      stage.abort();
      return false;
    }
    return stage.seal(bytes);
  }
  void abort() override { stage.abort(); }

 private:
  HalVerifiedFileStage stage;
};
}  // namespace companion
