#include "HalCompanionWifiRadio.h"

#include <Logging.h>
#include <esp_event.h>
#include <esp_netif_defaults.h>
#include <esp_wifi_default.h>

#include <cstring>

namespace companion {
HalCompanionWifiRadio::~HalCompanionWifiRadio() {
  if (!end()) LOG_ERR("CWIFI", "Radio resources remain at destruction");
}
void HalCompanionWifiRadio::clearConfiguration() {
  volatile uint8_t* bytes = reinterpret_cast<volatile uint8_t*>(&configuration);
  for (size_t at = 0; at < sizeof(configuration); ++at) bytes[at] = 0;
}
bool HalCompanionWifiRadio::fail(const char* operation, esp_err_t result) {
  LOG_ERR("CWIFI", "Radio %s failed: %d", operation, static_cast<int>(result));
  clearConfiguration();
  end();
  return false;
}
bool HalCompanionWifiRadio::end() {
  clearConfiguration();
  if (started) {
    const auto result = esp_wifi_stop();
    if (result != ESP_OK && result != ESP_ERR_WIFI_NOT_STARTED) {
      LOG_ERR("CWIFI", "Radio stop failed: %d", static_cast<int>(result));
      return false;
    }
    started = false;
  }
  if (initialized) {
    const auto result = esp_wifi_deinit();
    if (result != ESP_OK) {
      LOG_ERR("CWIFI", "Radio deinit failed: %d", static_cast<int>(result));
      return false;
    }
    initialized = false;
  }
  if (netif) {
    if (attached) {
      const auto result = esp_wifi_clear_default_wifi_driver_and_handlers(netif);
      // The SDK destroys the interface driver even if detaching reports an error.
      attached = false;
      if (result != ESP_OK) {
        LOG_ERR("CWIFI", "Radio interface detach failed: %d", static_cast<int>(result));
        return false;
      }
    }
    esp_netif_destroy(netif);
    netif = nullptr;
  }
  if (eventLoop) {
    const auto result = esp_event_loop_delete_default();
    if (result != ESP_OK) {
      LOG_ERR("CWIFI", "Radio event loop release failed: %d", static_cast<int>(result));
      return false;
    }
    eventLoop = false;
  }
  return true;
}
bool HalCompanionWifiRadio::begin(WifiNetworkMode mode, std::string_view ssid, std::string_view password) {
  if (ownsResources()) {
    LOG_ERR("CWIFI", "Radio already owned");
    return false;
  }
  const bool hotspot = mode == WifiNetworkMode::Hotspot;
  if (ssid.empty() || ssid.size() > 32 || ssid.find('\0') != std::string_view::npos ||
      (hotspot ? !validWifiNetworkDescription({mode, ssid, password})
               : mode != WifiNetworkMode::SavedNetwork || password.size() > 64 ||
                     password.find('\0') != std::string_view::npos)) {
    LOG_ERR("CWIFI", "Invalid radio credentials");
    return false;
  }
  wifi_mode_t existingMode;
  const auto existing = esp_wifi_get_mode(&existingMode);
  if (existing != ESP_ERR_WIFI_NOT_INIT) {
    LOG_ERR("CWIFI", "Wi-Fi belongs to another owner: %d", static_cast<int>(existing));
    return false;
  }
  auto result = esp_netif_init();
  if (result != ESP_OK) return fail("network initialization", result);
  result = esp_event_loop_create_default();
  if (result == ESP_OK)
    eventLoop = true;
  else if (result != ESP_ERR_INVALID_STATE)
    return fail("event loop", result);
  esp_netif_config_t interfaceConfig = ESP_NETIF_DEFAULT_WIFI_STA();
  if (hotspot) interfaceConfig = ESP_NETIF_DEFAULT_WIFI_AP();
  netif = esp_netif_new(&interfaceConfig);
  if (!netif) return fail("interface allocation", ESP_ERR_NO_MEM);
  // Attach registers the interface in the default-handler registry, even on failure.
  attached = true;
  result = hotspot ? esp_netif_attach_wifi_ap(netif) : esp_netif_attach_wifi_station(netif);
  if (result != ESP_OK) return fail("interface attach", result);
  result = hotspot ? esp_wifi_set_default_wifi_ap_handlers() : esp_wifi_set_default_wifi_sta_handlers();
  if (result != ESP_OK) return fail("interface handlers", result);
  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  init.nvs_enable = false;
  result = esp_wifi_init(&init);
  if (result != ESP_OK) return fail("driver initialization", result);
  initialized = true;
  result = esp_wifi_set_storage(WIFI_STORAGE_RAM);
  if (result != ESP_OK) return fail("RAM credential storage", result);
  result = esp_wifi_set_mode(hotspot ? WIFI_MODE_AP : WIFI_MODE_STA);
  if (result != ESP_OK) return fail("mode", result);
  clearConfiguration();
  if (hotspot) {
    std::memcpy(configuration.ap.ssid, ssid.data(), ssid.size());
    std::memcpy(configuration.ap.password, password.data(), password.size());
    configuration.ap.ssid_len = ssid.size();
    configuration.ap.channel = 1;
    configuration.ap.max_connection = 1;
    configuration.ap.authmode = WIFI_AUTH_WPA2_PSK;
  } else {
    std::memcpy(configuration.sta.ssid, ssid.data(), ssid.size());
    if (!password.empty()) std::memcpy(configuration.sta.password, password.data(), password.size());
    configuration.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    configuration.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  }
  result = esp_wifi_set_config(hotspot ? WIFI_IF_AP : WIFI_IF_STA, &configuration);
  clearConfiguration();
  if (result != ESP_OK) return fail("credentials", result);
  started = true;
  result = esp_wifi_start();
  if (result != ESP_OK) return fail("start", result);
  if (!hotspot) {
    result = esp_wifi_connect();
    if (result != ESP_OK) return fail("connect", result);
  }
  return true;
}
bool HalCompanionWifiRadio::address(std::array<uint8_t, 4>& output) const {
  if (!started || !netif) return false;
  esp_netif_ip_info_t info{};
  const auto result = esp_netif_get_ip_info(netif, &info);
  if (result != ESP_OK) {
    LOG_ERR("CWIFI", "Radio address failed: %d", static_cast<int>(result));
    return false;
  }
  std::array<uint8_t, 4> bytes{};
  std::memcpy(bytes.data(), &info.ip.addr, bytes.size());
  if (!bytes[0] || bytes[0] == 127 || bytes[0] >= 224) return false;
  output = bytes;
  return true;
}
}  // namespace companion
