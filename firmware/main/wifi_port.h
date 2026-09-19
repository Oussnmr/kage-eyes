#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Wi-Fi credentials live in the ESP32's encrypted-at-rest NVS partition,
 * never in the GitHub Pages installer or the firmware repository. */
void wifi_port_init(void);
void wifi_port_begin_setup(void);
bool wifi_port_connected(void);
bool wifi_port_setup_active(void);
void wifi_port_start_update(void);

#ifdef __cplusplus
}
#endif
