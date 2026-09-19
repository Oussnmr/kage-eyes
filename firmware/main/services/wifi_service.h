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

// Remote-backend authentication is provisioned locally over USB Serial/JTAG.
// The secret is stored only in Kage NVS and is never compiled into firmware.
bool wifi_service_has_api_key(void);
const char *wifi_service_api_key(void);
int wifi_service_active_profile_index(void);
