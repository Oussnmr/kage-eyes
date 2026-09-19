#include "mic_meter.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "bsp/esp-bsp.h"
#include "cJSON.h"
#include "esp_codec_dev.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "event_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kage_bridge.h"
#include "wifi_service.h"

namespace {
constexpr char TAG[] = "kage-mic";
constexpr char LOCAL_BACKEND_AUDIO_URL[] = "http://192.168.129.157:8000/audio";
constexpr char REMOTE_BACKEND_AUDIO_URL[] = "https://m920q.tailbf4c85.ts.net:8443/audio";

constexpr int SAMPLE_RATE = 16000;
constexpr int SAMPLE_COUNT = 256;
constexpr int MAX_RECORD_SECONDS = 8;
constexpr size_t MAX_RECORD_SAMPLES =
    static_cast<size_t>(SAMPLE_RATE) * MAX_RECORD_SECONDS;
constexpr int PRE_ROLL_MS = 320;
constexpr size_t PRE_ROLL_SAMPLES =
    static_cast<size_t>(SAMPLE_RATE) * PRE_ROLL_MS / 1000;
constexpr int SILENCE_MS = 700;
constexpr size_t SILENCE_SAMPLES =
    static_cast<size_t>(SAMPLE_RATE) * SILENCE_MS / 1000;
constexpr int MIN_PHRASE_MS = 350;
constexpr size_t MIN_PHRASE_SAMPLES =
    static_cast<size_t>(SAMPLE_RATE) * MIN_PHRASE_MS / 1000;
constexpr int START_CONFIRM_FRAMES = 3;
constexpr int POST_TIMEOUT_MS = 60000;
constexpr int POST_COOLDOWN_MS = 350;

static std::atomic<bool> s_active{false};
static std::atomic<float> s_level{0.0f};
static std::atomic<MicMeterState> s_state{MicMeterState::Off};
static TaskHandle_t s_task;
static esp_codec_dev_handle_t s_microphone;

static int16_t *s_recording;
static int16_t *s_pre_roll;
static size_t s_pre_roll_write;
static size_t s_pre_roll_filled;

struct AudioResponse {
    char body[192];
    size_t length;
};

static esp_err_t audio_response_event(esp_http_client_event_t *event) {
    auto *response = static_cast<AudioResponse *>(event->user_data);
    if (!response || event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) {
        return ESP_OK;
    }

    const size_t available = sizeof(response->body) - 1 - response->length;
    const size_t incoming = static_cast<size_t>(event->data_len);
    const size_t copy_len = incoming < available ? incoming : available;
    if (copy_len) {
        std::memcpy(response->body + response->length, event->data, copy_len);
        response->length += copy_len;
        response->body[response->length] = 0;
    }
    return ESP_OK;
}

static void *allocate_audio_buffer(size_t bytes) {
    void *buffer = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) {
        buffer = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    }
    return buffer;
}

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
    event_log_add("Microphone ready: %d Hz", SAMPLE_RATE);
    return true;
}

static float frame_rms(const int16_t *samples, size_t count) {
    if (!samples || count == 0) return 0.0f;

    double energy = 0.0;
    for (size_t i = 0; i < count; ++i) {
        const float sample = static_cast<float>(samples[i]) / 32768.0f;
        energy += sample * sample;
    }
    return static_cast<float>(std::sqrt(energy / count));
}

static float display_level_from_rms(float rms) {
    float level = (rms - 0.008f) * 11.0f;
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    return std::sqrt(level);
}

static void pre_roll_push(const int16_t *samples, size_t count) {
    if (!s_pre_roll || !samples || PRE_ROLL_SAMPLES == 0) return;

    for (size_t i = 0; i < count; ++i) {
        s_pre_roll[s_pre_roll_write] = samples[i];
        s_pre_roll_write = (s_pre_roll_write + 1) % PRE_ROLL_SAMPLES;
        if (s_pre_roll_filled < PRE_ROLL_SAMPLES) ++s_pre_roll_filled;
    }
}

static size_t pre_roll_copy(int16_t *destination, size_t capacity) {
    if (!destination || !s_pre_roll || s_pre_roll_filled == 0 || capacity == 0) {
        return 0;
    }

    size_t count = s_pre_roll_filled;
    if (count > capacity) count = capacity;

    const size_t start =
        (s_pre_roll_write + PRE_ROLL_SAMPLES - s_pre_roll_filled) % PRE_ROLL_SAMPLES;
    for (size_t i = 0; i < count; ++i) {
        destination[i] = s_pre_roll[(start + i) % PRE_ROLL_SAMPLES];
    }
    return count;
}

static bool append_recording(const int16_t *samples, size_t count, size_t *used) {
    if (!samples || !used || !s_recording) return false;
    if (*used >= MAX_RECORD_SAMPLES) return false;

    size_t available = MAX_RECORD_SAMPLES - *used;
    size_t copy_count = count < available ? count : available;
    std::memcpy(s_recording + *used, samples, copy_count * sizeof(int16_t));
    *used += copy_count;
    return copy_count == count;
}

static bool post_audio_url(const char *url, const int16_t *samples, size_t count,
                           bool remote) {
    const size_t bytes = count * sizeof(int16_t);

    esp_http_client_config_t config = {};
    config.url = url;
    config.timeout_ms = POST_TIMEOUT_MS;
    AudioResponse response = {};
    config.event_handler = audio_response_event;
    config.user_data = &response;
    if (remote) {
        config.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Could not create audio HTTP client");
        return false;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
    esp_http_client_set_header(client, "X-Kage-Audio-Format", "s16le-mono");
    esp_http_client_set_header(client, "X-Kage-Sample-Rate", "16000");

    const char *api_key = wifi_service_api_key();
    if (api_key && api_key[0]) {
        esp_http_client_set_header(client, "X-Kage-Key", api_key);
    }

    esp_http_client_set_post_field(
        client,
        reinterpret_cast<const char *>(samples),
        static_cast<int>(bytes));

    const esp_err_t result = esp_http_client_perform(client);
    const int status =
        result == ESP_OK ? esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);

    if (result != ESP_OK) {
        ESP_LOGW(TAG, "%s audio upload failed: %s",
                 remote ? "Remote" : "Local", esp_err_to_name(result));
        return false;
    }

    if (status != 200) {
        ESP_LOGW(TAG, "%s audio backend returned HTTP %d",
                 remote ? "Remote" : "Local", status);
        return false;
    }

    cJSON *root = cJSON_Parse(response.body);
    cJSON *command_item = root ? cJSON_GetObjectItem(root, "command") : nullptr;
    cJSON *sequence_item = root ? cJSON_GetObjectItem(root, "sequence") : nullptr;
    if (cJSON_IsString(command_item) && cJSON_IsNumber(sequence_item)) {
        kage_bridge_apply_command(
            command_item->valuestring,
            static_cast<uint32_t>(sequence_item->valuedouble));
    } else {
        ESP_LOGW(TAG, "Audio backend response missing command");
    }
    if (root) cJSON_Delete(root);

    return true;
}

static bool post_audio_to_backend(const int16_t *samples, size_t count) {
    if (!samples || count < MIN_PHRASE_SAMPLES) return false;
    if (!wifi_service_connected()) {
        ESP_LOGW(TAG, "Voice captured but Wi-Fi is offline");
        event_log_add("Voice skipped: Wi-Fi offline");
        return false;
    }

    const size_t bytes = count * sizeof(int16_t);
    ESP_LOGI(TAG, "Uploading %u audio bytes to M920q",
             static_cast<unsigned>(bytes));
    event_log_add("Voice upload: %u KB",
                  static_cast<unsigned>((bytes + 1023) / 1024));

    WifiServiceInfo wifi = {};
    wifi_service_get_info(&wifi);
    const bool on_local_lan = std::strncmp(wifi.ip, "192.168.129.", 13) == 0;

    if (!on_local_lan && !wifi_service_has_api_key()) {
        ESP_LOGW(TAG, "Remote voice disabled: no API key");
        event_log_add("Voice remote: key missing");
        return false;
    }

    // Stop /command/latest polling from competing with the voice POST. If a
    // GET is already in flight, wait for that one request to finish; once this
    // flag is raised the bridge will not start another GET.
    if (!kage_bridge_voice_upload_begin(9000)) {
        ESP_LOGW(TAG, "Voice upload could not acquire network slot");
        event_log_add("Voice upload blocked by GET");
        return false;
    }

    const int64_t upload_start_us = esp_timer_get_time();
    bool ok = false;
    if (on_local_lan) {
        ok = post_audio_url(LOCAL_BACKEND_AUDIO_URL, samples, count, false);
        if (!ok && wifi_service_has_api_key()) {
            event_log_add("Voice: local failed, trying remote");
            ok = post_audio_url(REMOTE_BACKEND_AUDIO_URL, samples, count, true);
        }
    } else {
        ok = post_audio_url(REMOTE_BACKEND_AUDIO_URL, samples, count, true);
        if (!ok) {
            event_log_add("Voice: remote failed");
        }
    }

    kage_bridge_voice_upload_end();

    const double upload_ms = (esp_timer_get_time() - upload_start_us) / 1000.0;
    ESP_LOGI(TAG, "Voice upload/response: %.1f ms", upload_ms);
    event_log_add("Voice upload/response: %.0f ms", upload_ms);

    if (!ok) {
        event_log_add("Voice upload failed");
        return false;
    }

    ESP_LOGI(TAG, "Audio processed by M920q");
    event_log_add("Voice processed by M920q");
    return true;
}

static void microphone_task(void *) {
    int16_t samples[SAMPLE_COUNT];

    s_recording = static_cast<int16_t *>(
        allocate_audio_buffer(MAX_RECORD_SAMPLES * sizeof(int16_t)));
    s_pre_roll = static_cast<int16_t *>(
        allocate_audio_buffer(PRE_ROLL_SAMPLES * sizeof(int16_t)));

    if (!s_recording || !s_pre_roll) {
        s_state.store(MicMeterState::Error);
        ESP_LOGE(TAG, "Could not allocate voice buffers");
        event_log_add("Microphone buffer allocation failed");
        vTaskDelete(nullptr);
        return;
    }

    bool initialized = false;
    bool speaking = false;
    size_t recording_samples = 0;
    size_t silent_samples = 0;
    int start_confirm = 0;
    float noise_floor = 0.008f;
    float envelope = 0.0f;
    int64_t capture_start_us = 0;

    /* app_shell also initializes devices on the shared I2C bus during boot.
       Let that finish before the codec claims the bus. */
    vTaskDelay(pdMS_TO_TICKS(600));

    while (true) {
        if (!initialized) {
            initialized = open_microphone();
            if (!initialized) {
                s_state.store(MicMeterState::Error);
                ESP_LOGE(TAG, "Microphone initialization failed");
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        }

        const int result = esp_codec_dev_read(s_microphone, samples, sizeof(samples));
        if (result != ESP_CODEC_DEV_OK) {
            s_state.store(MicMeterState::Error);
            s_level.store(0.0f);
            ESP_LOGW(TAG, "Microphone read failed: %d", result);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        const float rms = frame_rms(samples, SAMPLE_COUNT);
        const float level = display_level_from_rms(rms);
        const float follow = level > envelope ? 0.48f : 0.12f;
        envelope += (level - envelope) * follow;
        s_level.store(envelope, std::memory_order_relaxed);
        s_state.store(
            s_active.load(std::memory_order_relaxed)
                ? MicMeterState::Listening
                : MicMeterState::Off,
            std::memory_order_relaxed);

        if (!speaking) {
            pre_roll_push(samples, SAMPLE_COUNT);

            const float start_threshold =
                noise_floor * 2.8f > 0.018f ? noise_floor * 2.8f : 0.018f;

            if (rms < start_threshold) {
                noise_floor = noise_floor * 0.985f + rms * 0.015f;
            }

            if (rms >= start_threshold) {
                ++start_confirm;
            } else {
                start_confirm = 0;
            }

            if (start_confirm >= START_CONFIRM_FRAMES) {
                recording_samples =
                    pre_roll_copy(s_recording, MAX_RECORD_SAMPLES);
                silent_samples = 0;
                speaking = true;
                capture_start_us = esp_timer_get_time();
                start_confirm = 0;
                ESP_LOGI(TAG, "Voice start (noise %.4f, rms %.4f)",
                         noise_floor, rms);
                event_log_add("Voice detected");
            }
            continue;
        }

        const bool fully_appended =
            append_recording(samples, SAMPLE_COUNT, &recording_samples);

        const float silence_threshold =
            noise_floor * 1.8f > 0.012f ? noise_floor * 1.8f : 0.012f;
        if (rms < silence_threshold) {
            silent_samples += SAMPLE_COUNT;
        } else {
            silent_samples = 0;
        }

        const bool end_of_phrase =
            recording_samples >= MIN_PHRASE_SAMPLES &&
            silent_samples >= SILENCE_SAMPLES;
        const bool reached_limit =
            !fully_appended || recording_samples >= MAX_RECORD_SAMPLES;

        if (!end_of_phrase && !reached_limit) continue;

        ESP_LOGI(TAG, "Voice end: %.2f s%s",
                 static_cast<double>(recording_samples) / SAMPLE_RATE,
                 reached_limit ? " (limit)" : "");
        event_log_add("Voice captured: %.1f s",
                      static_cast<double>(recording_samples) / SAMPLE_RATE);

        const int64_t speech_end_us = esp_timer_get_time();
        event_log_add("Voice endpoint: %.0f ms capture",
                      capture_start_us > 0
                          ? (speech_end_us - capture_start_us) / 1000.0
                          : 0.0);

        post_audio_to_backend(s_recording, recording_samples);

        speaking = false;
        recording_samples = 0;
        silent_samples = 0;
        start_confirm = 0;
        s_pre_roll_write = 0;
        s_pre_roll_filled = 0;

        /* During the blocking POST the robot is not recording. A short extra
           cooldown also prevents the tail of a nearby speaker from retriggering. */
        vTaskDelay(pdMS_TO_TICKS(POST_COOLDOWN_MS));
    }
}
}  // namespace

void mic_meter_begin() {
    if (s_task) return;
    xTaskCreatePinnedToCore(
        microphone_task, "mic-meter", 7168, nullptr, 4, &s_task, 0);
}

void mic_meter_set_active(bool active) {
    s_active.store(active, std::memory_order_relaxed);
    if (!active) {
        s_state.store(MicMeterState::Off, std::memory_order_relaxed);
    } else if (s_state.load(std::memory_order_relaxed) == MicMeterState::Off) {
        s_state.store(MicMeterState::Starting, std::memory_order_relaxed);
    }
}

float mic_meter_level() {
    return s_level.load(std::memory_order_relaxed);
}

MicMeterState mic_meter_state() {
    return s_state.load(std::memory_order_relaxed);
}
