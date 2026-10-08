#pragma once

#include <HalStorage.h>
#include <Logging.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <climits>

#include "CompanionDictionaryBundleBuilder.h"
#include "CompanionInventoryPaths.h"

namespace companion {
// Session-owned outside the task stack. Four HAL wrappers are allocated once
// on first use and retained across dictionary scans; all member I/O is locked.
class HalDictionaryBundleSource final : public DictionaryBundleSource {
 public:
  ~HalDictionaryBundleSource() override { close(); }
  bool begin(std::string_view base, bool compressedData, bool hasSynonyms) {
    if (!close()) return false;
    snapshotReady = false;
    if (base.size() > path.size() - 9 || !validInventoryPath(base)) return fail("Invalid dictionary base path");
    if (base.data() != path.data()) std::copy(base.begin(), base.end(), path.begin());
    baseLength = base.size();
    compressed = compressedData;
    synonyms = hasSynonyms;
    count = synonyms ? 4 : 3;
    reads = 0;
    failed = false;
    for (unsigned member = 0; member < count; ++member) {
      const auto suffix = member == 0 && compressed ? std::string_view(".dict.dz") : SUFFIXES[member];
      std::copy(suffix.begin(), suffix.end(), path.begin() + baseLength);
      path[baseLength + suffix.size()] = 0;
      if (!Storage.openFileForReadReusing("COMPANION", path.data(), files[member]) || files[member].isDirectory()) {
        fail("Opening dictionary member failed");
        close();
        return false;
      }
      lengths[member] = files[member].fileSize64();
    }
    snapshotReady = true;
    return true;
  }
  // Reopen after validation closes its handles. The caller must retain stable
  // member ownership; matching lengths do not detect same-length mutation.
  bool reopen() {
    if (!snapshotReady) return fail("No dictionary snapshot to reopen");
    const auto expected = lengths;
    const auto members = count;
    if (baseLength == 0 || !begin(std::string_view(path.data(), baseLength), compressed, synonyms)) return false;
    for (unsigned member = 0; member < members; ++member) {
      if (lengths[member] != expected[member]) {
        fail("Dictionary member length changed");
        close();
        return false;
      }
    }
    return true;
  }
  bool size(unsigned member, uint64_t& bytes) override {
    if (!usable(member)) return false;
    const auto actual = files[member].fileSize64();
    if (actual != lengths[member]) return fail("Dictionary member length changed");
    bytes = actual;
    return true;
  }
  bool read(unsigned member, uint64_t at, std::span<uint8_t> bytes) override {
    if (!usable(member)) return false;
    if (bytes.size() > INT_MAX || at > lengths[member] || bytes.size() > lengths[member] - at)
      return fail("Dictionary read bounds failed");
    if (bytes.empty()) return true;
    if (!files[member].seek64(at) || files[member].read(bytes.data(), bytes.size()) != static_cast<int>(bytes.size()))
      return fail("Dictionary member read failed");
    if (++reads == 32) {
      reads = 0;
      vTaskDelay(1);
    }
    return true;
  }
  bool close() override {
    bool ok = true;
    for (unsigned member = files.size(); member > 0; --member)
      if (files[member - 1].isOpen() && !files[member - 1].close()) ok = false;
    failed = true;
    if (!ok) {
      snapshotReady = false;
      LOG_ERR("COMPANION", "Closing dictionary members failed");
    }
    return ok;
  }

 private:
  static constexpr std::array<std::string_view, 4> SUFFIXES = {".dict", ".idx", ".ifo", ".syn"};
  std::array<HalFile, 4> files;
  std::array<uint64_t, 4> lengths{};
  std::array<char, 512> path{};
  size_t baseLength = 0;
  unsigned count = 0;
  uint8_t reads = 0;
  bool compressed = false, synonyms = false, failed = true, snapshotReady = false;
  bool usable(unsigned member) {
    return (!failed && member < count && files[member].isOpen()) || fail("Dictionary source is unavailable");
  }
  bool fail(const char* reason) {
    failed = true;
    snapshotReady = false;
    LOG_ERR("COMPANION", "%s", reason);
    return false;
  }
};
}  // namespace companion
