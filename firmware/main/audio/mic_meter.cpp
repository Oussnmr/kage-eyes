#include "mic_meter.h"

#include <atomic>
#include <cmath>
#include <cstdint>

#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr char TAG[] = "kage-mic";
constexpr int SAMPLE_RATE = 22050;
constexpr int SAMPLE_COUNT = 256;

static std::atomic<bool> s_active{false};
static std::atomic<float> s_level{0.0f};
static std::atomic<MicMeterState> s_state{MicMeterState::Off};
static TaskHandle_t s_task;
static esp_codec_dev_handle_t s_microphone;

static bool open_microphone() {
    s_state.store(MicMeterState::Starting);
    s_microphone = bsp_audio_codec_microphone_init();
    if (!s_microphone) return false;

    esp_codec_dev_sample_info_t format = {};
    format.sample_rate = SAMPLE_RATE;
    format.channel = 1;
    format.bits_per_sample = 16;
    if (esp_codec_dev_open(s_microphone, &format) != ESP_CODEC_DEV_OK) return false;
    if (esp_codec_dev_set_in_gain(s_microphone, 30.0f) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "Could not set microphone gain; continuing with codec default");
    }
    ESP_LOGI(TAG, "ES8311 microphone ready: %d Hz mono", SAMPLE_RATE);
    return true;
}

static void microphone_task(void *) {
    int16_t samples[SAMPLE_COUNT];
    bool initialized = false;
    float envelope = 0.0f;

    while (true) {
        if (!s_active.load(std::memory_order_relaxed)) {
            envelope *= 0.75f;
            s_level.store(envelope, std::memory_order_relaxed);
            vTaskDelay(pdMS_TO_TICKS(40));
            continue;
        }

        if (!initialized) {
            initialized = open_microphone();
            if (!initialized) {
                s_state.store(MicMeterState::Error);
                ESP_LOGE(TAG, "Microphone initialization failed");
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            s_state.store(MicMeterState::Listening);
        }

        const int result = esp_codec_dev_read(s_microphone, samples, sizeof(samples));
        if (result != ESP_CODEC_DEV_OK) {
            s_state.store(MicMeterState::Error);
            s_level.store(0.0f);
            ESP_LOGW(TAG, "Microphone read failed: %d", result);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        s_state.store(MicMeterState::Listening);

        double energy = 0.0;
        for (int i = 0; i < SAMPLE_COUNT; ++i) {
            const float sample = static_cast<float>(samples[i]) / 32768.0f;
            energy += sample * sample;
        }
        float rms = static_cast<float>(std::sqrt(energy / SAMPLE_COUNT));
        /* Remove the analog noise floor, then use a gentle curve so normal speech
           remains visible without letting a clap pin every dot at full size. */
        float level = (rms - 0.008f) * 11.0f;
        if (level < 0.0f) level = 0.0f;
        if (level > 1.0f) level = 1.0f;
        level = std::sqrt(level);
        const float follow = level > envelope ? 0.48f : 0.12f;
        envelope += (level - envelope) * follow;
        s_level.store(envelope, std::memory_order_relaxed);
    }
}
}  // namespace

void mic_meter_begin() {
    if (s_task) return;
    xTaskCreatePinnedToCore(microphone_task, "mic-meter", 6144, nullptr, 4, &s_task, 0);
}

void mic_meter_set_active(bool active) {
    s_active.store(active, std::memory_order_relaxed);
    if (!active) s_state.store(MicMeterState::Off, std::memory_order_relaxed);
}

float mic_meter_level() {
    return s_level.load(std::memory_order_relaxed);
}

MicMeterState mic_meter_state() {
    return s_state.load(std::memory_order_relaxed);
}

