#pragma once

#include <stdbool.h>
#include <stdint.h>

struct WifiServiceInfo {
    bool configured;
    bool connected;
    char ssid[33];
    char ip[16];
    char gateway[16];
    char mask[16];
    char bssid[18];
    int rssi;
    int channel;
    int32_t last_disconnect_reason;
};

void wifi_service_begin(void);
bool wifi_service_connected(void);
bool wifi_service_is_configured(void);
void wifi_service_forget(void);
const char *wifi_service_name(void);
int32_t wifi_service_last_disconnect_reason(void);
void wifi_service_get_info(WifiServiceInfo *info);
