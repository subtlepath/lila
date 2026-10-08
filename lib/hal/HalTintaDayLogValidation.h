#pragma once

#include <HalStorage.h>
#include <Logging.h>

#include "CompanionTintaDayLog.h"

namespace companion {
// Caller owns the open handle, excludes writers and verifies the snapshot hash.
inline bool validateTintaDayLogFile(HalFile& file, std::span<uint8_t> scratch, bool (*progress)(void*) = nullptr,
                                    void* context = nullptr) {
  const auto failure = [](const char* reason) {
    LOG_ERR("COMPANION", "Tinta day snapshot validation failed: %s", reason);
    return false;
  };
  if (scratch.size() < 12 || !file.isOpen() || file.isDirectory()) return failure("arguments");
  const uint64_t length = file.fileSize64();
  if (length > UINT32_MAX || (progress && !progress(context))) return failure("length or cancelled");
  if (!file.seek64(0) || file.read(scratch.data(), 4) != 4) return failure("header read");
  TintaDayLogValidator validator;
  if (!validator.begin(scratch.first(4), static_cast<uint32_t>(length))) return failure("header");
  for (uint64_t offset = 4; offset < length; offset += 12) {
    if ((offset - 4) % (12 * 32) == 0 && progress && !progress(context)) return failure("cancelled");
    if (file.read(scratch.data(), 12) != 12 || !validator.record(scratch.first(12))) return failure("record");
  }
  if (file.fileSize64() != length || !validator.finish()) return failure("extent or totals");
  return true;
}
}  // namespace companion
