#pragma once

#include <stdbool.h>

#include "driver/i2c_master.h"

/* Read-only AXP2101 fuel-gauge monitor. It never changes charger settings. */
bool battery_monitor_begin(i2c_master_bus_handle_t bus);
bool battery_monitor_read(float *level, bool *charging);
