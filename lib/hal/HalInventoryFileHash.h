#pragma once

#include <HalStorage.h>

#include "CompanionRecords.h"

namespace companion {
// Borrows an open regular file and scratch; leaves the file at EOF on success.
// Outputs are unchanged on failure. No file handle or buffer allocation.
using InventoryHashProgress = bool (*)(void*);
bool hashInventoryFile(HalFile& file, std::span<uint8_t> scratch, uint64_t& length, Digest& hash,
                       InventoryHashProgress progress = nullptr, void* context = nullptr);
}  // namespace companion
