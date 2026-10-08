#pragma once
#include "esp_wifi.h"
struct esp_netif_t {};
struct esp_netif_config_t {
  int mode;
};
struct esp_netif_ip_info_t {
  struct {
    uint32_t addr;
  } ip;
};
esp_err_t esp_netif_init();
esp_netif_t* esp_netif_new(const esp_netif_config_t*);
void esp_netif_destroy(esp_netif_t*);
esp_err_t esp_netif_get_ip_info(esp_netif_t*, esp_netif_ip_info_t*);
