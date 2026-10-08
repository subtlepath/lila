#pragma once

#include <Logging.h>
#include <mbedtls/sha256.h>

#include "CompanionDictionaryBundleBuilder.h"
#include "HalInventoryFileHash.h"

namespace companion {
// Retained outside the task stack. Hashes canonical builder output without SD writes.
class HalDictionaryBundleHashStage final : public DictionaryBundleStage {
 public:
  explicit HalDictionaryBundleHashStage(InventoryHashProgress progress = nullptr, void* context = nullptr)
      : progress(progress), context(context) {
    mbedtls_sha256_init(&hash);
  }
  HalDictionaryBundleHashStage(const HalDictionaryBundleHashStage&) = delete;
  HalDictionaryBundleHashStage& operator=(const HalDictionaryBundleHashStage&) = delete;
  ~HalDictionaryBundleHashStage() override { mbedtls_sha256_free(&hash); }
  bool begin() override {
    abort();
    if ((progress && !progress(context)) || mbedtls_sha256_starts(&hash, 0) != 0) return failure("begin");
    active = true;
    return true;
  }
  bool write(uint64_t offset, std::span<const uint8_t> bytes) override {
    if (!active || offset != length || bytes.size() > UINT32_MAX - length || (progress && !progress(context)) ||
        (!bytes.empty() && mbedtls_sha256_update(&hash, bytes.data(), bytes.size()) != 0))
      return failure("write order/bounds/hash");
    length += bytes.size();
    return true;
  }
  bool seal(uint64_t bytes) override {
    if (!active || bytes != length || bytes < 22 || (progress && !progress(context)) ||
        mbedtls_sha256_finish(&hash, digest.data()) != 0)
      return failure("seal");
    active = false;
    sealed = true;
    return true;
  }
  void abort() override {
    active = sealed = false;
    length = 0;
    digest = {};
  }
  const Digest* contentHash() const { return sealed ? &digest : nullptr; }

 private:
  bool failure(const char* reason) {
    abort();
    LOG_ERR("COMPANION", "Dictionary canonical hash stage failed: %s", reason);
    return false;
  }
  mbedtls_sha256_context hash;
  InventoryHashProgress progress;
  void* context;
  Digest digest{};
  uint64_t length = 0;
  bool active = false, sealed = false;
};
}  // namespace companion
