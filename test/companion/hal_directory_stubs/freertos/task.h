#pragma once
#include "HalStorage.h"
inline void vTaskDelay(unsigned) { ++directory_test::state.yields; }
