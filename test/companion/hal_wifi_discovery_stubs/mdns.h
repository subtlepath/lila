#pragma once
#include <cstddef>
#include <cstdint>
using esp_err_t = int;
inline constexpr esp_err_t ESP_OK = 0;
struct mdns_txt_item_t;
esp_err_t mdns_init();
void mdns_free();
esp_err_t mdns_hostname_set(const char* name);
esp_err_t mdns_service_add(const char* name, const char* service, const char* proto, uint16_t port,
                           mdns_txt_item_t* txt, size_t count);
