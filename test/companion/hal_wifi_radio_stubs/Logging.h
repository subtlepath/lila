#pragma once
template <class... Args>
inline void radioTestLog(Args&&...) {}
#define LOG_ERR(...) radioTestLog(__VA_ARGS__)
