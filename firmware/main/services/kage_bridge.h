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

// Triggered by the robot face's three-tap gesture. It asks the PC backend to
// start a direct listening session, or stop the existing Kage Voice process.
void kage_bridge_toggle_voice(void);
void kage_bridge_interrupt_voice(void);

// Apply the command returned directly by /audio. This avoids waiting for the
// next /command/latest polling cycle after a successful voice upload.
void kage_bridge_apply_command(const char *command, uint32_t sequence);

// Prevent command polling from overlapping the much larger /audio upload.
// Voice gets priority: once requested, the bridge will not start another GET
// until the upload releases the shared network slot.
bool kage_bridge_voice_upload_begin(uint32_t timeout_ms);
void kage_bridge_voice_upload_end(void);
