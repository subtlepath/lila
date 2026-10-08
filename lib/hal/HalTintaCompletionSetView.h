#pragma once

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>

#include "../Serialization/BinaryRecordBytes.h"
#include "CompanionTintaCompletionSet.h"

namespace companion {
// Borrowed handle; caller excludes writers and binds the verified file to its course/frontier.
class HalTintaCompletionSetView {
 public:
  explicit HalTintaCompletionSetView(HalFile& file) : file(file) {}
  bool begin(TintaCompletionKind kind, std::span<uint8_t> scratch, bool (*progress)(void*) = nullptr,
             void* context = nullptr) {
    ready = false;
    count = extent = 0;
    if (scratch.size() < 12 || !file.isOpen() || file.isDirectory()) return failure("arguments");
    const uint64_t length = file.fileSize64();
    if (length > UINT32_MAX || (progress && !progress(context))) return failure("length or cancelled");
    if (!file.seek64(0) || file.read(scratch.data(), 12) != 12) return failure("header read");
    TintaCompletionSetValidator validator;
    if (!validator.begin(scratch.first(12), static_cast<uint32_t>(length), kind)) return failure("header");
    const uint32_t entries = binary_record::getU32(scratch.data() + 8);
    const uint32_t chunkEntries = std::min<size_t>(32, scratch.size() / 4);
    for (uint32_t first = 0; first < entries;) {
      if (progress && !progress(context)) return failure("cancelled");
      const uint32_t n = std::min(chunkEntries, entries - first);
      if (file.read(scratch.data(), n * 4) != static_cast<int>(n * 4)) return failure("identity read");
      for (uint32_t i = 0; i < n; ++i)
        if (!validator.identity(scratch.subspan(i * 4, 4))) return failure("identity");
      first += n;
    }
    if (file.read(scratch.data(), 4) != 4 || !validator.finish(scratch.first(4)) || file.fileSize64() != length)
      return failure("checksum or extent");
    count = entries;
    extent = static_cast<uint32_t>(length);
    validatedKind = kind;
    ready = true;
    return true;
  }
  bool entryCount(TintaCompletionKind kind, uint32_t& output) {
    if (!ready || validatedKind != kind || !file.isOpen() || file.fileSize64() != extent)
      return failure("count arguments or extent");
    output = count;
    return true;
  }
  bool identityAt(uint32_t index, uint32_t& output) {
    if (!ready || !file.isOpen() || file.fileSize64() != extent || index >= count)
      return failure("index arguments or extent");
    uint8_t bytes[4];
    if (!file.seek64(12 + index * 4) || file.read(bytes, sizeof bytes) != sizeof bytes) return failure("index read");
    const uint32_t identity = binary_record::getU32(bytes);
    if (!identity || identity == UINT32_MAX) return failure("index identity");
    output = identity;
    return true;
  }
  bool contains(uint32_t identity, bool& found) {
    if (!ready || !file.isOpen() || file.fileSize64() != extent || !identity || identity == UINT32_MAX)
      return failure("lookup arguments or extent");
    uint32_t first = 0, last = count;
    uint8_t bytes[4];
    while (first < last) {
      const uint32_t middle = first + (last - first) / 2;
      if (!file.seek64(12 + middle * 4) || file.read(bytes, sizeof bytes) != sizeof bytes)
        return failure("lookup read");
      const uint32_t value = binary_record::getU32(bytes);
      if (value == identity) {
        found = true;
        return true;
      }
      if (value < identity)
        first = middle + 1;
      else
        last = middle;
    }
    found = false;
    return true;
  }

 private:
  bool failure(const char* reason) {
    ready = false;
    LOG_ERR("COMPANION", "Tinta completion view failed: %s", reason);
    return false;
  }
  HalFile& file;
  uint32_t count = 0;
  uint32_t extent = 0;
  TintaCompletionKind validatedKind = TintaCompletionKind::Lessons;
  bool ready = false;
};
}  // namespace companion
