#pragma once

#include "lib/hal/HalMemory.h"

namespace companion_memory_test {
inline HalMemory::HeapStats internal{1024 * 1024, 1024 * 1024, 1024 * 1024, 1024 * 1024};
}

inline HalMemory::HeapStats HalMemory::getInternalHeap() { return companion_memory_test::internal; }
