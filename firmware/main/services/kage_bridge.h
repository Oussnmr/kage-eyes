#pragma once

#include <stdbool.h>
#include <stdint.h>

struct KageBridgeInfo {
    bool reachable;
    int http_status;
    uint32_t sequence;
    char command[16];
    char error[48];
};

void kage_bridge_begin(void);
void kage_bridge_get_info(KageBridgeInfo *info);
