#pragma once

#include <stdbool.h>
#include <stdint.h>

void wifi_service_begin(void);
bool wifi_service_connected(void);
bool wifi_service_is_configured(void);
void wifi_service_forget(void);
const char *wifi_service_name(void);
int32_t wifi_service_last_disconnect_reason(void);
