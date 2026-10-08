#pragma once

#include <HalStorage.h>
#include <Logging.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "CompanionTransfer.h"
#include "CompanionZipRangeValidation.h"
#include "HalCompanionFileLookup.h"

namespace companion {
class HalZipRangeStorage final : public ZipRangeStorage {
 public:
  static constexpr const char* PATH = "/.crosspoint/companion/zip-ranges-next";
  static constexpr const char* NAME_INDEX_PATH = "/.crosspoint/companion/zip-names-index-next";
  enum class Purpose { Ranges, NameIndex };
  explicit HalZipRangeStorage(Purpose purpose = Purpose::Ranges)
      : path(purpose == Purpose::NameIndex ? NAME_INDEX_PATH : PATH) {}
  ~HalZipRangeStorage() override { discard(); }
  bool reset() override {
    failed = true;
    sealed = false;
    operations = 0;
    if (!discard() || !Storage.ready() || !Storage.ensureDirectoryExists(TRANSFER_DIRECTORY) ||
        lookup.inspect(path) != CompanionFilePresence::Missing)
      return failure("prepare");
    owned = true;
    if (!Storage.openFileForWriteReusing("COMPANION", path, file) || file.isDirectory()) return failure("open");
    extent = 0;
    failed = false;
    return true;
  }
  bool read(uint64_t at, std::span<uint8_t> bytes) override {
    if (failed || sealed || !file || bytes.size() != 16 || at % 16 || at > extent || bytes.size() > extent - at ||
        !file.seek64(at) || file.read(bytes.data(), bytes.size()) != 16)
      return failure("read");
    yieldPeriodically();
    return true;
  }
  bool write(uint64_t at, std::span<const uint8_t> bytes) override {
    if (failed || sealed || !file || bytes.size() != 16 || at % 16 || at > extent || at > MAX_BYTES - 16 ||
        !file.seek64(at) || file.write(bytes.data(), bytes.size()) != 16 || !file.seek64(at) ||
        file.read(readback.data(), readback.size()) != 16 || !std::equal(bytes.begin(), bytes.end(), readback.begin()))
      return failure("write/readback");
    extent = std::max<uint64_t>(extent, at + 16);
    yieldPeriodically();
    return true;
  }
  bool seal(uint64_t bytes) override {
    if (failed || sealed || !file || bytes != extent || !file.truncate(bytes) || !file.sync() ||
        file.fileSize64() != bytes || !file.close())
      return failure("seal");
    sealed = true;
    return true;
  }
  // The sorted stage is disposable, including after seal. It is never handed
  // off as an installation receipt; only this owner's file may be removed.
  bool discard() {
    failed = true;
    sealed = false;
    if (file.isOpen() && !file.close()) return failure("cleanup close");
    if (!owned) return true;
    const auto presence = lookup.inspect(path, false);
    if (presence == CompanionFilePresence::Error ||
        (presence == CompanionFilePresence::Present && !Storage.remove(path)))
      return failure("cleanup remove");
    owned = false;
    extent = 0;
    return true;
  }

 private:
  static constexpr uint64_t MAX_BYTES = 20000ULL * 16;
  const char* const path;
  HalFile file;
  HalCompanionFileLookup lookup;
  std::array<uint8_t, 16> readback{};
  uint64_t extent = 0;
  uint8_t operations = 0;
  bool failed = true, owned = false, sealed = false;
  bool failure(const char* operation) {
    failed = true;
    LOG_ERR("COMPANION", "ZIP range stage %s failed", operation);
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
