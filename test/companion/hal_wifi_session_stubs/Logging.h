#pragma once
extern unsigned wifiDiscoveryErrors;
template <class... Args>
inline void wifiSessionTestLog(Args&&...) {
  ++wifiDiscoveryErrors;
}
#define LOG_ERR(...) wifiSessionTestLog(__VA_ARGS__)
