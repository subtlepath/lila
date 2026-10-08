#pragma once
extern unsigned wifiDiscoveryErrors;
#define LOG_ERR(...) (++wifiDiscoveryErrors)
