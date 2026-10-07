#ifndef HOST_ESP_WIFI_H
#define HOST_ESP_WIFI_H

#include <cstdint>

#define ESP_OK 0
typedef struct { int8_t rssi; } wifi_sta_info_t;
typedef struct { wifi_sta_info_t sta[10]; int num; } wifi_sta_list_t;
int esp_wifi_ap_get_sta_list(wifi_sta_list_t* list);

#endif
