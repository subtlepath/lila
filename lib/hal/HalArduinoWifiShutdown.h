#pragma once

#include <Logging.h>
#include <WiFi.h>

namespace companion {
inline bool arduinoWifiActive() { return WiFi.getMode() != WIFI_OFF; }
// Caller has stopped Arduino networking services and owns Wi-Fi exclusively.
inline bool shutdownArduinoWifi() {
  if (WiFi.mode(WIFI_OFF) && WiFi.getMode() == WIFI_OFF) return true;
  LOG_ERR("CWIFI", "Arduino Wi-Fi shutdown failed");
  return false;
}
}  // namespace companion
