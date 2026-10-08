#pragma once
#include "CompanionInventoryPublication.h"
namespace companion {
InventoryPublicationResult recoverInventorySnapshots(TransferStorage& storage, const Identity& generation,
                                                     std::span<uint8_t> scratch);
}  // namespace companion
