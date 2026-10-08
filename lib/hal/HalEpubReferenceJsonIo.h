#pragma once

#include <HalStorage.h>
#include <Logging.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/sha256.h>

#include <climits>

#include "CompanionEpubReferenceJson.h"

namespace companion {
using ReferenceJsonGuard = bool (*)(void*);
// These adapters borrow one HAL handle and scratch. The enclosing snapshot owner
// must close the handle on every outcome; adapters never own or publish a path.
class HalEpubReferenceJsonReader final : public EpubReferenceJsonReader {
 public:
  HalEpubReferenceJsonReader() { mbedtls_sha256_init(&hashContext); }
  HalEpubReferenceJsonReader(const HalEpubReferenceJsonReader&) = delete;
  HalEpubReferenceJsonReader& operator=(const HalEpubReferenceJsonReader&) = delete;
  ~HalEpubReferenceJsonReader() override { mbedtls_sha256_free(&hashContext); }
  bool begin(HalFile& source, std::span<uint8_t> buffer, ReferenceJsonGuard authorize, void* context) {
    file = &source;
    scratch = buffer;
    guard = authorize;
    owner = context;
    failed = true;
    at = count = 0;
    fetched = 0;
    fills = 0;
    finalized = false;
    if (!file->isOpen() || file->isDirectory() || scratch.empty() || scratch.size() > INT_MAX || !guard ||
        !guard(owner))
      return failure("prepare");
    length = file->fileSize64();
    if (!length || length > REMOVAL_METADATA_MAX_BYTES || !file->seek64(0) || !guard(owner) ||
        mbedtls_sha256_starts(&hashContext, 0))
      return failure("extent/seek");
    failed = false;
    return true;
  }
  int read() override {
    if (failed) return -1;
    if (at == count) {
      if (!guard(owner) || !file->isOpen() || file->fileSize64() != length) {
        failure("ownership/extent");
        return -1;
      }
      if (fetched == length) return -1;
      count = static_cast<size_t>(std::min<uint64_t>(scratch.size(), length - fetched));
      if (file->read(scratch.data(), count) != static_cast<int>(count) ||
          mbedtls_sha256_update(&hashContext, scratch.data(), count) || !guard(owner)) {
        failure("read/ownership");
        return -1;
      }
      at = 0;
      fetched += count;
      if (++fills == 16) {
        fills = 0;
        vTaskDelay(1);
      }
    }
    return scratch[at++];
  }
  size_t readBytes(char* output, size_t bytes) override {
    if (!output && bytes) {
      failure("null output");
      return 0;
    }
    size_t copied = 0;
    while (copied < bytes) {
      const int byte = read();
      if (byte < 0) break;
      output[copied++] = static_cast<char>(byte);
    }
    return copied;
  }
  bool contentHash(Digest& output) {
    if (!healthy() || fetched != length || at != count) return failure("incomplete hash input");
    if (!finalized && mbedtls_sha256_finish(&hashContext, actual.data())) return failure("SHA finish");
    finalized = true;
    output = actual;
    return true;
  }
  bool healthy() const override {
    return !failed && file && file->isOpen() && file->fileSize64() == length && guard && guard(owner);
  }

 private:
  HalFile* file = nullptr;
  std::span<uint8_t> scratch;
  ReferenceJsonGuard guard = nullptr;
  void* owner = nullptr;
  uint64_t length = 0, fetched = 0;
  size_t at = 0, count = 0;
  uint8_t fills = 0;
  mbedtls_sha256_context hashContext;
  Digest actual{};
  bool failed = true, finalized = false;
  bool failure(const char* reason) {
    failed = true;
    LOG_ERR("COMPANION", "Reference JSON read failed: %s", reason);
    return false;
  }
};
class HalEpubReferenceJsonWriter final : public EpubReferenceJsonWriter {
 public:
  HalEpubReferenceJsonWriter() { mbedtls_sha256_init(&hashContext); }
  HalEpubReferenceJsonWriter(const HalEpubReferenceJsonWriter&) = delete;
  HalEpubReferenceJsonWriter& operator=(const HalEpubReferenceJsonWriter&) = delete;
  ~HalEpubReferenceJsonWriter() override { mbedtls_sha256_free(&hashContext); }
  const Digest* contentHash() const { return finished && !failed ? &actual : nullptr; }
  bool begin(HalFile& destination, std::span<uint8_t> buffer, uint64_t expectedBytes, ReferenceJsonGuard authorize,
             void* context) {
    file = &destination;
    scratch = buffer;
    expected = expectedBytes;
    guard = authorize;
    owner = context;
    extent = 0;
    pending = 0;
    writes = 0;
    failed = true;
    finished = false;
    if (!file->isOpen() || file->isDirectory() || scratch.empty() || !expected ||
        expected > REMOVAL_METADATA_MAX_BYTES || !guard || !guard(owner) || !file->seek64(0) || !guard(owner) ||
        mbedtls_sha256_starts(&hashContext, 0))
      return failure("prepare");
    failed = false;
    return true;
  }
  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* bytes, size_t count) override {
    if (failed || finished) return 0;
    if ((!bytes && count) || !guard(owner) || count > expected - extent - pending) {
      failure("bounds/ownership");
      return 0;
    }
    const size_t accepted = count;
    while (count) {
      const size_t part = std::min(count, scratch.size() - pending);
      std::copy_n(bytes, part, scratch.data() + pending);
      pending += part;
      bytes += part;
      count -= part;
      if (pending == scratch.size() && !flush()) return 0;
    }
    return accepted;
  }
  // The owner still checks close and reopens/hashes the complete candidate before
  // persisting its immutable declaration. Flush alone does not seal a snapshot.
  bool finish() {
    if (failed || finished || extent + pending != expected || !flush() || !guard(owner) || !file->truncate(expected) ||
        !guard(owner) || !file->sync() || !guard(owner) || file->fileSize64() != expected ||
        mbedtls_sha256_finish(&hashContext, actual.data()))
      return failure("finish/truncate/sync");
    finished = true;
    return true;
  }

 private:
  HalFile* file = nullptr;
  std::span<uint8_t> scratch;
  ReferenceJsonGuard guard = nullptr;
  void* owner = nullptr;
  uint64_t expected = 0, extent = 0;
  size_t pending = 0;
  uint8_t writes = 0;
  mbedtls_sha256_context hashContext;
  Digest actual{};
  bool failed = true, finished = false;
  bool flush() {
    if (failed || !file->isOpen() || !guard(owner)) return failure("flush ownership");
    if (!pending) return true;
    if (file->write(scratch.data(), pending) != pending ||
        mbedtls_sha256_update(&hashContext, scratch.data(), pending) || !guard(owner))
      return failure("flush write/ownership");
    extent += pending;
    pending = 0;
    if (++writes == 16) {
      writes = 0;
      vTaskDelay(1);
    }
    return true;
  }
  bool failure(const char* reason) {
    failed = true;
    LOG_ERR("COMPANION", "Reference JSON write failed: %s", reason);
    return false;
  }
};
}  // namespace companion
