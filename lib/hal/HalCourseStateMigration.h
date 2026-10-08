#pragma once

#if LILA_TINTA
#include <span>

#include "CompanionRecords.h"

namespace companion {
class HalTransferStorage;
// Caller must close Tinta state handles and exclude state writers until this returns.
bool prepareMigratedCourseState(HalTransferStorage& storage, const Identity& course, std::span<uint8_t> scratch);
bool selectActiveCourseState(HalTransferStorage& storage, std::span<uint8_t> scratch, Identity& course, bool& bound);
}  // namespace companion
#endif
