#pragma once

enum class MicMeterState {
    Off,
    Starting,
    Listening,
    Processing,
    Error,
};

/* Starts a dormant worker. The codec is initialized only when the mic app opens. */
void mic_meter_begin();
void mic_meter_set_active(bool active);
float mic_meter_level();
MicMeterState mic_meter_state();

