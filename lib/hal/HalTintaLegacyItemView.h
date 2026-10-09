#pragma once

#include <HalStorage.h>
#include <Logging.h>

#include <span>

#include "core/srs/ProgressHeader.h"

namespace companion {
// Borrows an immutable reviewed file and workspace. The caller verifies its hash
// and excludes writers; this view never runs local recovery or changes the file.
class HalTintaLegacyItemView final {
 public:
  explicit HalTintaLegacyItemView(HalFile& file) : file(file) {}
  bool begin(std::span<uint8_t> workspace, bool (*permitted)(void*) = nullptr, void* context = nullptr) {
    ready = false;
    scratch = workspace;
    permission = permitted;
    permissionContext = context;
    if (!allowed() || !file.isOpen() || file.isDirectory() || scratch.size() < 160 || overlapsOwner())
      return failure("arguments");
    length = file.fileSize64();
    if (length < 1024 || (length - 1024) % 16) return failure("extent");
    if (!file.seek64(0) || file.read(scratch.data(), 80) != 80 || !allowed()) return failure("header A read");
    const bool first = tinta::core::ProgressHeader::decode(scratch.data(), selected);
    if (!file.seek64(512) || file.read(scratch.data() + 80, 80) != 80 || !allowed()) return failure("header B read");
    tinta::core::ProgressHeader other;
    const bool second = tinta::core::ProgressHeader::decode(scratch.data() + 80, other);
    if (!first && !second) return failure("headers");
    if (first && second && selected.seq == other.seq && std::memcmp(scratch.data(), scratch.data() + 80, 80))
      return failure("ambiguous headers");
    if (second && (!first || other.seq > selected.seq)) selected = other;
    const uint64_t records = (length - 1024) / 16;
    // Local recovery can restore the final pending record after a torn append.
    if (records < selected.recordCount &&
        !(selected.pendingValid && records + 1 == selected.recordCount && selected.pendingSlot == records))
      return failure("missing records");
    ready = file.fileSize64() == length && allowed();
    return ready || failure("changed extent");
  }
  uint32_t count() const { return ready && allowed() ? selected.recordCount : 0; }
  const tinta::core::ProgressHeader* header() const { return ready && allowed() ? &selected : nullptr; }
  bool record(uint32_t slot, tinta::core::ItemState& output) {
    if (!ready || !allowed() || slot >= selected.recordCount || file.fileSize64() != length)
      return failure("record arguments");
    if (selected.pendingValid && slot == selected.pendingSlot) {
      output = selected.pending;
      return true;
    }
    if (!file.seek64(1024 + uint64_t(slot) * 16) || file.read(scratch.data(), 16) != 16 || !allowed() ||
        file.fileSize64() != length)
      return failure("record read");
    if (!tinta::core::ItemState::decode(scratch.data(), output)) return failure("record bytes");
    return true;
  }

 private:
  HalFile& file;
  std::span<uint8_t> scratch;
  tinta::core::ProgressHeader selected;
  uint64_t length = 0;
  bool (*permission)(void*) = nullptr;
  void* permissionContext = nullptr;
  bool ready = false;
  bool overlapsOwner() const {
    const auto start = reinterpret_cast<uintptr_t>(scratch.data());
    const auto owner = reinterpret_cast<uintptr_t>(this);
    return start <= owner ? owner - start < scratch.size() : start - owner < sizeof(*this);
  }
  bool allowed() const { return !permission || permission(permissionContext); }
  bool failure([[maybe_unused]] const char* reason) {
    ready = false;
    LOG_ERR("COMPANION", "Legacy Tinta item inspection failed: %s", reason);
    return false;
  }
};
}  // namespace companion
