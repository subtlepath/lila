#pragma once
#include "HalStorage.h"
inline void vTaskDelay(unsigned) { ++inventory_hal_test::state.yields; }
