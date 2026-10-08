#pragma once

#include <HalStorage.h>
#include <Logging.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>

#include "CompanionInventoryPathsBuilder.h"
#include "CompanionTransfer.h"

namespace companion {
// One serialized snapshot owner controls this candidate and its publication.
class HalInventorySnapshotStage : public InventoryPathsStage {
 public:
  enum class Kind { Index, Paths };
  static constexpr char INDEX_CANDIDATE[] = "/.crosspoint/companion/inventory-next";
  static constexpr char PATH_CANDIDATE[] = "/.crosspoint/companion/inventory-paths-next";
  explicit HalInventorySnapshotStage(Kind kind) : kind(kind) {}
  const char* candidatePath() const { return kind == Kind::Index ? INDEX_CANDIDATE : PATH_CANDIDATE; }
  ~HalInventorySnapshotStage() override { abort(); }
  bool begin() override {
    failed = true;
    writing = false;
    if (kind != Kind::Index && kind != Kind::Paths) return failure("candidate kind");
    if (!discard()) return false;
    if (!Storage.ready() || !Storage.ensureDirectoryExists(TRANSFER_DIRECTORY)) return failure("prepare");
    owned = true;
    file = Storage.open(candidatePath(), O_WRONLY | O_CREAT | O_TRUNC);
    if (!file || file.isDirectory()) return failure("open");
    extent = 0;
    writesSinceYield = 0;
    failed = false;
    writing = true;
    return true;
  }
  bool write(uint64_t offset, std::span<const uint8_t> bytes) override {
    if (failed || !writing || !file || offset > extent || bytes.size() > UINT64_MAX - offset)
      return failure("write bounds");
    if (!file.seek64(offset) || file.write(bytes.data(), bytes.size()) != bytes.size()) return failure("write");
    extent = std::max<uint64_t>(extent, offset + bytes.size());
    if (++writesSinceYield == 32) {
      writesSinceYield = 0;
      vTaskDelay(1);
    }
    return true;
  }
  bool seal(uint64_t bytes) override {
    if (failed || !writing || !file || bytes != extent) return failure("seal bounds");
    if (!file.truncate(bytes) || !file.sync() || file.fileSize64() != bytes) return failure("sync");
    if (!file.close()) return failure("close");
    writing = false;
    owned = false;
    // The coordinator now owns the sealed candidate, including crash recovery.
    return true;
  }
  void abort() override {
    failed = true;
    writing = false;
    discard();
  }

 private:
  const Kind kind;
  HalFile file;
  uint64_t extent = 0;
  uint8_t writesSinceYield = 0;
  bool writing = false, failed = true, owned = false;
  bool failure([[maybe_unused]] const char* operation) {
    failed = true;
    LOG_ERR("COMPANION", "Inventory snapshot stage %s failed", operation);
    return false;
  }
  bool discard() {
    if (file && !file.close()) return failure("cleanup close");
    if (!owned) return true;
    if (!Storage.ready()) return failure("cleanup SD unavailable");
    if (Storage.exists(candidatePath()) && !Storage.remove(candidatePath())) return failure("cleanup remove");
    owned = false;
    extent = 0;
    return true;
  }
};
}  // namespace companion
