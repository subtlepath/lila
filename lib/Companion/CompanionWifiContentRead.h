#pragma once

#include "CompanionContentExportBinding.h"

namespace companion {
inline constexpr size_t WIFI_CONTENT_READ_REQUEST_SIZE = 16 + CONTENT_READ_REQUEST_SIZE;
inline bool decodeWifiContentRead(std::span<const uint8_t> input, const Identity& transaction,
                                  const Identity& installation, uint64_t revision, const ContentExportBinding& binding,
                                  ContentReadRequest& output) {
  if (input.size() != WIFI_CONTENT_READ_REQUEST_SIZE ||
      !std::equal(transaction.begin(), transaction.end(), input.begin()))
    return false;
  ContentReadRequest parsed;
  if (!decodeContentReadRequest(input.subspan(16), parsed) ||
      !binding.permits(transaction, installation, revision, parsed))
    return false;
  output = parsed;
  return true;
}
}  // namespace companion
