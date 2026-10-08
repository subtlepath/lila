#pragma once
#include "HalStorage.h"
template <class... T>
inline void directoryLog(T&&...) {
  ++directory_test::state.errors;
}
#define LOG_ERR(...) directoryLog(__VA_ARGS__)
