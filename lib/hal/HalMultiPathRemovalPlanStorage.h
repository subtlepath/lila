#pragma once

#include <Logging.h>
#include <mbedtls/sha256.h>

#include "CompanionMultiPathRemovalPlan.h"
#include "HalCompanionFileLookup.h"

namespace companion {
enum class MultiPathRemovalStorageResult { Ok, Missing, Invalid, Corrupt, IoError };
// Off-stack owner. The caller excludes all plan writers throughout iteration.
class HalMultiPathRemovalPlanStorage final : private InventoryIndexStorage {
 public:
  HalMultiPathRemovalPlanStorage() : reader(*this, buffer) { mbedtls_sha256_init(&hash); }
  ~HalMultiPathRemovalPlanStorage() {
    close();
    mbedtls_sha256_free(&hash);
  }
  HalMultiPathRemovalPlanStorage(const HalMultiPathRemovalPlanStorage&) = delete;
  HalMultiPathRemovalPlanStorage& operator=(const HalMultiPathRemovalPlanStorage&) = delete;
  const MultiPathRemovalPlanHeader* current() const { return ready ? reader.current() : nullptr; }
  const Digest* verifiedDigest() const { return ready ? &expected : nullptr; }
  MultiPathRemovalStorageResult open(const Digest& digest, const ContentRemovalRequest& request) {
    ready = false;
    if (!validContentRemovalRequest(request) ||
        !std::any_of(digest.begin(), digest.end(), [](uint8_t b) { return b != 0; }))
      return MultiPathRemovalStorageResult::Invalid;
    expected = digest;
    failedIo = false;
    if (!close()) return io("previous close");
    static constexpr char PREFIX[] = "/.crosspoint/companion/removal-plan-";
    static constexpr char HEX_DIGITS[] = "0123456789abcdef";
    size_t at = sizeof(PREFIX) - 1;
    std::copy_n(PREFIX, at, target.begin());
    for (uint8_t byte : expected) {
      target[at++] = HEX_DIGITS[byte >> 4];
      target[at++] = HEX_DIGITS[byte & 15];
    }
    target[at] = 0;
    const auto presence = lookup.inspect(target.data());
    if (presence == CompanionFilePresence::Missing) return MultiPathRemovalStorageResult::Missing;
    if (presence != CompanionFilePresence::Present ||
        !Storage.openFileForReadReusing("COMPANION", target.data(), file) || file.isDirectory())
      return io("open");
    length = file.fileSize64();
    if (!reader.open(request) || !verifyHash()) {
      const bool closed = close();
      if (!closed || failedIo) return io("plan verification");
      LOG_ERR("COMPANION", "Multi-path removal plan is corrupt or has a different request");
      return MultiPathRemovalStorageResult::Corrupt;
    }
    reader.rewind();
    ready = true;
    return MultiPathRemovalStorageResult::Ok;
  }
  InventoryPathRecordResult next(std::span<char> output) {
    if (!ready) return InventoryPathRecordResult::Error;
    const auto result = reader.next(output);
    if (result == InventoryPathRecordResult::Error || (result == InventoryPathRecordResult::End && !verifyHash())) {
      ready = false;
      LOG_ERR("COMPANION", "Multi-path removal plan iteration failed");
      return InventoryPathRecordResult::Error;
    }
    return result;
  }
  bool rewind() {
    if (!ready || !verifyHash()) {
      ready = false;
      return false;
    }
    reader.rewind();
    return true;
  }
  bool close() {
    ready = false;
    if (!file.isOpen() || file.close()) return true;
    return failure("close");
  }

 private:
  std::array<uint8_t, MULTI_PATH_REMOVAL_RECORD_MAX> buffer{};
  std::array<char, 112> target{};
  MultiPathRemovalPlanReader reader;
  HalFile file;
  HalCompanionFileLookup lookup;
  mbedtls_sha256_context hash;
  Digest expected{}, actual{};
  uint64_t length = 0;
  bool ready = false, failedIo = false;
  bool size(uint64_t& output) override {
    if (!file.isOpen() || file.isDirectory()) return false;
    output = file.fileSize64();
    return true;
  }
  bool read(uint64_t offset, std::span<uint8_t> output) override {
    if (!file.isOpen() || !file.seek64(offset) ||
        file.read(output.data(), output.size()) != static_cast<int>(output.size()))
      return failure("record read");
    vTaskDelay(1);
    return true;
  }
  bool verifyHash() {
    if (!file.isOpen() || file.fileSize64() != length) return false;
    if (!file.seek64(0) || mbedtls_sha256_starts(&hash, 0)) return failure("hash preparation");
    uint64_t remaining = length;
    while (remaining) {
      const auto count = static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size()));
      if (file.read(buffer.data(), count) != static_cast<int>(count) ||
          mbedtls_sha256_update(&hash, buffer.data(), count))
        return failure("hash read");
      remaining -= count;
      vTaskDelay(1);
    }
    if (mbedtls_sha256_finish(&hash, actual.data())) return failure("hash completion");
    if (actual != expected || file.fileSize64() != length) return false;
    return file.sync() || failure("hash sync");
  }
  bool failure(const char* operation) {
    failedIo = true;
    LOG_ERR("COMPANION", "Multi-path removal plan read %s failed", operation);
    return false;
  }
  MultiPathRemovalStorageResult io(const char* operation) {
    ready = false;
    LOG_ERR("COMPANION", "Multi-path removal plan read %s failed", operation);
    return MultiPathRemovalStorageResult::IoError;
  }
};
}  // namespace companion
