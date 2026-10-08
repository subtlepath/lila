#pragma once
#include <cstdint>
using esp_err_t = int;
inline constexpr int ESP_OK = 0, ESP_ERR_WIFI_NOT_INIT = 1, ESP_ERR_INVALID_STATE = 2, ESP_ERR_NO_MEM = 3,
                     ESP_ERR_WIFI_NOT_STARTED = 4;
enum wifi_mode_t { WIFI_MODE_NULL, WIFI_MODE_STA, WIFI_MODE_AP };
enum wifi_interface_t { WIFI_IF_STA, WIFI_IF_AP };
inline constexpr int WIFI_STORAGE_RAM = 1, WIFI_AUTH_WPA2_PSK = 2, WIFI_ALL_CHANNEL_SCAN = 3,
                     WIFI_CONNECT_AP_BY_SIGNAL = 4;
struct wifi_config_t {
  struct {
    uint8_t ssid[32], password[64];
    int ssid_len, channel, max_connection, authmode;
  } ap{};
  struct {
    uint8_t ssid[32], password[64];
    int scan_method, sort_method;
  } sta{};
};
struct wifi_init_config_t {
  bool nvs_enable;
};
#define WIFI_INIT_CONFIG_DEFAULT() {true}
esp_err_t esp_wifi_get_mode(wifi_mode_t*);
esp_err_t esp_wifi_init(const wifi_init_config_t*);
esp_err_t esp_wifi_deinit();
esp_err_t esp_wifi_stop();
esp_err_t esp_wifi_start();
esp_err_t esp_wifi_connect();
esp_err_t esp_wifi_set_storage(int);
esp_err_t esp_wifi_set_mode(wifi_mode_t);
esp_err_t esp_wifi_set_config(wifi_interface_t, const wifi_config_t*);
