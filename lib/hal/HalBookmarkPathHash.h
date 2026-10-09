#pragma once

#include <HalStorage.h>
#include <mbedtls/sha256.h>

#include "HalFilenameCodec.h"

namespace companion {
inline bool updateBookmarkPathHash(mbedtls_sha256_context& sha, std::string_view path) {
  return hal_filename::writeFolded(
      path, +[](uint32_t value) { return Storage.foldFilenameCodepoint(value); },
      +[](void* context, const uint8_t* bytes, size_t size) {
        return mbedtls_sha256_update(static_cast<mbedtls_sha256_context*>(context), bytes, size) == 0;
      },
      &sha);
}
}  // namespace companion
