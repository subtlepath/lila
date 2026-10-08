#pragma once

#include <HalStorage.h>
#include <Logging.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/sha256.h>

#include <climits>

#include "CompanionTransfer.h"
#include "HalCompanionFileLookup.h"
#include "HalInventoryFileHash.h"

namespace companion {
// Session-owned verified private staging. Candidate paths are borrowed until
// the next begin and must remain valid throughout writing/cleanup.
class HalVerifiedFileStage final {
 public:
  using Progress = InventoryHashProgress;
  explicit HalVerifiedFileStage(std::span<uint8_t> readbackScratch, Progress progress = nullptr,
                                void* context = nullptr, const char* parentPath = TRANSFER_DIRECTORY)
      : lookup(progress, context, parentPath),
        scratch(readbackScratch),
        progress(progress),
        context(context),
        parentPath(parentPath) {
    mbedtls_sha256_init(&digest);
  }
  ~HalVerifiedFileStage() {
    abort();
    mbedtls_sha256_free(&digest);
  }
  HalVerifiedFileStage(const HalVerifiedFileStage&) = delete;
  HalVerifiedFileStage& operator=(const HalVerifiedFileStage&) = delete;
  // Only valid after seal succeeds; no candidate has been installed/published.
  const Digest& contentHash() const { return hash; }
  bool isSealed() const { return sealed; }
  bool begin(const char* candidate, uint64_t maxBytes) {
    failed = true;
    writing = sealed = false;
    if (!discard()) return false;
    path = candidate;
    limit = maxBytes;
    if (!path || scratch.size() < 64 || scratch.size() > INT_MAX || !Storage.ready() || !parentPath ||
        !Storage.ensureDirectoryExists(parentPath))
      return failure("prepare");
    if (progress && !progress(context)) return failure("cancelled");
    // Recovery/publication owns pre-existing candidates, including sealed ones.
    if (lookup.inspect(path) != CompanionFilePresence::Missing) return failure("candidate unavailable");
    if (mbedtls_sha256_starts(&digest, 0) != 0) return failure("SHA start");
    owned = true;
    if (!Storage.openFileForWriteReusing("COMPANION", path, file) || file.isDirectory()) return failure("open");
    extent = syncedExtent = 0;
    hasSynced = false;
    writes = 0;
    failed = false;
    writing = true;
    return true;
  }
  // Caller proves durable ownership; final seal still checks the complete content hash.
  bool resume(const char* candidate, uint64_t maxBytes, uint64_t durableOffset) {
    failed = true;
    writing = sealed = false;
    if (!discard()) return false;
    path = candidate;
    limit = maxBytes;
    if (!path || scratch.size() < 64 || scratch.size() > INT_MAX || durableOffset > limit ||
        lookup.inspect(path) != CompanionFilePresence::Present)
      return failure("resume arguments or lookup");
    file = Storage.open(path, O_RDWR);
    if (!file || file.isDirectory() || file.fileSize64() < durableOffset || mbedtls_sha256_starts(&digest, 0) != 0 ||
        !file.seek64(0))
      return failure("resume open or extent");
    uint64_t at = 0;
    uint8_t reads = 0;
    while (at < durableOffset) {
      if (progress && !progress(context)) return failure("cancelled");
      const auto count = static_cast<size_t>(std::min<uint64_t>(scratch.size(), durableOffset - at));
      if (file.read(scratch.data(), count) != static_cast<int>(count) ||
          mbedtls_sha256_update(&digest, scratch.data(), count) != 0)
        return failure("resume prefix read/SHA");
      at += count;
      if (++reads == 32) {
        reads = 0;
        vTaskDelay(1);
      }
    }
    if (!file.truncate(durableOffset) || !file.sync() || file.fileSize64() != durableOffset ||
        !file.seek64(durableOffset))
      return failure("resume trim/sync/seek");
    owned = true;
    extent = syncedExtent = durableOffset;
    hasSynced = true;
    writes = 0;
    failed = false;
    writing = true;
    return true;
  }
  bool write(uint64_t at, std::span<const uint8_t> bytes) {
    if (failed || !writing || !file || at != extent || extent > limit || bytes.size() > limit - extent)
      return failure("write bounds");
    if (bytes.empty()) return true;
    if (progress && !progress(context)) return failure("cancelled");
    if (file.write(bytes.data(), bytes.size()) != bytes.size() ||
        mbedtls_sha256_update(&digest, bytes.data(), bytes.size()) != 0)
      return failure("write or SHA update");
    extent += bytes.size();
    if (++writes == 32) {
      writes = 0;
      vTaskDelay(1);
    }
    return true;
  }
  bool syncPending(uint64_t& durableOffset) {
    if (failed || !writing || !file || file.fileSize64() != extent) return failure("pending sync state or extent");
    if (progress && !progress(context)) return failure("cancelled");
    if (!file.sync() || file.fileSize64() != extent) return failure("pending sync");
    syncedExtent = extent;
    hasSynced = true;
    durableOffset = extent;
    return true;
  }
  // durableOffset must already be committed by the owning receive journal.
  bool retainPending(uint64_t durableOffset) {
    if (!owned || !file || !hasSynced || durableOffset > syncedExtent || file.fileSize64() < durableOffset)
      return failure("retain pending bounds");
    owned = false;
    writing = false;
    failed = true;
    // Recovery retains the file even if close acknowledgement is lost.
    if (!file.close()) return failure("retain pending close");
    return true;
  }
  bool seal(uint64_t bytes, const Digest* requiredHash = nullptr) {
    if (failed || !writing || !file || bytes != extent) return failure("seal bounds");
    if (progress && !progress(context)) return failure("cancelled");
    if (!file.truncate(bytes) || !file.sync() || file.fileSize64() != bytes) return failure("sync");
    Digest expected{}, actual{};
    uint64_t length = 0;
    if (mbedtls_sha256_finish(&digest, expected.data()) != 0 ||
        !hashInventoryFile(file, scratch, length, actual, progress, context) || length != bytes || actual != expected)
      return failure("SHA readback");
    if (requiredHash && actual != *requiredHash) return failure("manifest SHA mismatch");
    if (!file.close()) return failure("close");
    hash = actual;
    writing = false;
    owned = false;
    sealed = true;
    return true;
  }
  bool cleanup() {
    abort();
    return !owned && !file.isOpen();
  }
  void abort() {
    failed = true;
    writing = false;
    discard();
  }

 private:
  HalFile file;
  HalCompanionFileLookup lookup;
  const char* path = nullptr;
  uint64_t limit = 0;
  mbedtls_sha256_context digest;
  std::span<uint8_t> scratch;
  Progress progress;
  void* context;
  const char* parentPath;
  Digest hash{};
  uint64_t extent = 0, syncedExtent = 0;
  uint8_t writes = 0;
  bool failed = true, writing = false, owned = false, sealed = false, hasSynced = false;
  bool failure(const char* operation) {
    failed = true;
    LOG_ERR("COMPANION", "Verified file stage %s failed", operation);
    return false;
  }
  bool discard() {
    if (file.isOpen() && !file.close()) return failure("cleanup close");
    if (!owned) return true;
    if (!Storage.ready()) return failure("cleanup SD unavailable");
    const auto presence = lookup.inspect(path, false);
    if (presence == CompanionFilePresence::Error ||
        (presence == CompanionFilePresence::Present && !Storage.remove(path)))
      return failure("cleanup remove");
    owned = false;
    extent = 0;
    return true;
  }
};
}  // namespace companion
