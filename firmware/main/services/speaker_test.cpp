#include "speaker_test.h"

#include <cmath>
#include <cstdint>

#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "event_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr char TAG[] = "kage-speaker";
constexpr int SAMPLE_RATE = 16000;
constexpr int TONE_HZ = 660;
constexpr int TONE_MS = 500;
constexpr int SAMPLE_COUNT = SAMPLE_RATE * TONE_MS / 1000;
constexpr float PI = 3.14159265358979323846f;
static int16_t s_tone[SAMPLE_COUNT];

static void close_and_delete(esp_codec_dev_handle_t device) {
    if (!device) return;
    esp_codec_dev_close(device);
    esp_codec_dev_delete(device);
}
}  // namespace

void speaker_test_play() {
    event_log_add("Speaker test starting");
    esp_codec_dev_handle_t speaker = bsp_audio_codec_speaker_init();
    if (!speaker) {
        ESP_LOGE(TAG, "Speaker initialization failed");
        event_log_add("Speaker test: init failed");
        return;
    }

    esp_codec_dev_sample_info_t format = {};
    format.sample_rate = SAMPLE_RATE;
    format.channel = 1;
    format.bits_per_sample = 16;
    if (esp_codec_dev_open(speaker, &format) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Speaker codec open failed");
        event_log_add("Speaker test: open failed");
        close_and_delete(speaker);
        return;
    }

    esp_codec_dev_set_out_vol(speaker, 80.0f);
    for (int index = 0; index < SAMPLE_COUNT; ++index) {
        const float phase = 2.0f * PI * TONE_HZ * index / SAMPLE_RATE;
        const float fade = index < 160
            ? static_cast<float>(index) / 160.0f
            : (index > SAMPLE_COUNT - 160
                ? static_cast<float>(SAMPLE_COUNT - index) / 160.0f : 1.0f);
        s_tone[index] = static_cast<int16_t>(std::sin(phase) * fade * 12000.0f);
    }

    const int result = esp_codec_dev_write(speaker, s_tone, sizeof(s_tone));
    if (result == ESP_CODEC_DEV_OK) {
        ESP_LOGI(TAG, "Speaker test played");
        event_log_add("Speaker test played");
    } else {
        ESP_LOGE(TAG, "Speaker test write failed: %d", result);
        event_log_add("Speaker test: write failed");
    }
    /* esp_codec_dev_write hands the buffer to the I2S DMA. Keep the codec
       open until the DMA has had time to physically play the tone. */
    vTaskDelay(pdMS_TO_TICKS(TONE_MS + 80));
    close_and_delete(speaker);
}
