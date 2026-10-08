#pragma once
#include "esp_netif.h"
esp_err_t esp_netif_attach_wifi_ap(esp_netif_t*);
esp_err_t esp_netif_attach_wifi_station(esp_netif_t*);
esp_err_t esp_wifi_set_default_wifi_ap_handlers();
esp_err_t esp_wifi_set_default_wifi_sta_handlers();
esp_err_t esp_wifi_clear_default_wifi_driver_and_handlers(void*);
