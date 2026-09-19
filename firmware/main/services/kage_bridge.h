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

// Prevent command polling from overlapping the much larger /audio upload.
// Voice gets priority: once requested, the bridge will not start another GET
// until the upload releases the shared network slot.
bool kage_bridge_voice_upload_begin(uint32_t timeout_ms);
void kage_bridge_voice_upload_end(void);
