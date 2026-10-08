#pragma once

#include <HalStorage.h>
#include <Logging.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <climits>

#include "CompanionTransfer.h"
#include "CompanionZipNameDuplicateValidation.h"
#include "HalCompanionFileLookup.h"

namespace companion {
class HalZipNameBytesStorage final : public ZipNameBytesStorage {
 public:
  static constexpr const char* PATH = "/.crosspoint/companion/zip-names-bytes-next";
  ~HalZipNameBytesStorage() override { discard(); }
  bool reset() override {
    failed = true;
    sealed = false;
    operations = 0;
    if (!discard() || !Storage.ready() || !Storage.ensureDirectoryExists(TRANSFER_DIRECTORY) ||
        lookup.inspect(PATH) != CompanionFilePresence::Missing)
      return failure("prepare");
    owned = true;
    if (!Storage.openFileForWriteReusing("COMPANION", PATH, file) || file.isDirectory()) return failure("open");
    extent = 0;
    failed = false;
    return true;
  }
  bool read(uint64_t at, std::span<uint8_t> bytes) override {
    if (failed || sealed || !file || bytes.size() > INT_MAX || at > extent || bytes.size() > extent - at)
      return failure("read bounds");
    if (bytes.empty()) return true;
    if (!file.seek64(at) || file.read(bytes.data(), bytes.size()) != static_cast<int>(bytes.size()))
      return failure("read");
    yieldPeriodically();
    return true;
  }
  bool append(uint64_t at, std::span<const uint8_t> bytes) override {
    if (failed || sealed || !file || at != extent || bytes.empty() || bytes.size() > 3072 || extent > MAX_BYTES ||
        bytes.size() > MAX_BYTES - extent || !file.seek64(at) || file.write(bytes.data(), bytes.size()) != bytes.size())
      return failure("append");
    for (size_t offset = 0; offset < bytes.size();) {
      const auto count = std::min<size_t>(readback.size(), bytes.size() - offset);
      if (!file.seek64(at + offset) || file.read(readback.data(), count) != static_cast<int>(count) ||
          !std::equal(bytes.begin() + offset, bytes.begin() + offset + count, readback.begin()))
        return failure("append readback");
      offset += count;
      yieldPeriodically();
    }
    extent += bytes.size();
    return true;
  }
  bool seal(uint64_t bytes) override {
    if (failed || sealed || !file || bytes != extent || !file.truncate(bytes) || !file.sync() ||
        file.fileSize64() != bytes || !file.close())
      return failure("seal");
    sealed = true;
    return true;
  }
  // The private stage is disposable, including after seal. It is never handed
  // off as an installation receipt; only this owner's file may be removed.
  bool discard() {
    failed = true;
    sealed = false;
    if (file.isOpen() && !file.close()) return failure("cleanup close");
    if (!owned) return true;
    const auto presence = lookup.inspect(PATH, false);
    if (presence == CompanionFilePresence::Error ||
        (presence == CompanionFilePresence::Present && !Storage.remove(PATH)))
      return failure("cleanup remove");
    owned = false;
    extent = 0;
    return true;
  }

 private:
  static constexpr uint64_t MAX_BYTES = 20000ULL * 3072;
  HalFile file;
  HalCompanionFileLookup lookup;
  std::array<uint8_t, 16> readback{};
  uint64_t extent = 0;
  uint8_t operations = 0;
  bool failed = true, owned = false, sealed = false;
  bool failure(const char* operation) {
    failed = true;
    LOG_ERR("COMPANION", "ZIP name-byte stage %s failed", operation);
    return false;
  }
  void yieldPeriodically() {
    if (++operations == 32) {
      operations = 0;
      vTaskDelay(1);
    }
  }
};
}  // namespace companion
