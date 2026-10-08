#pragma once
#include "HalStorage.h"
template <class... Args>
inline void transferTestLog(Args&&...) {
  ++inventory_hal_test::state.errors;
}
#define LOG_ERR(...) transferTestLog(__VA_ARGS__)
#define LOG_DBG(...) ((void)0)
