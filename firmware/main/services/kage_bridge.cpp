#include "kage_bridge.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "eyes/robot_eyes.h"
#include "event_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "robot_motion.h"
#include "wifi_service.h"

namespace {
constexpr const char *LOCAL_BACKEND_URL = "http://192.168.129.157:8000/command/latest";
constexpr const char *REMOTE_BACKEND_URL = "https://m920q.tailbf4c85.ts.net:8443/command/latest";
constexpr const char *LOCAL_TOGGLE_URL = "http://192.168.129.157:8000/voice/toggle";
constexpr const char *LOCAL_INTERRUPT_URL = "http://192.168.129.157:8000/voice/interrupt";
constexpr const char *LOCAL_WAKE_URL = "http://192.168.129.157:8000/voice/wake";
constexpr const char *LOCAL_SLEEP_URL = "http://192.168.129.157:8000/voice/sleep";
constexpr const char *LOCAL_HOLD_START_URL = "http://192.168.129.157:8000/voice/hold/start";
constexpr const char *LOCAL_HOLD_STOP_URL = "http://192.168.129.157:8000/voice/hold/stop";
constexpr TickType_t LOCAL_POLL_DELAY = pdMS_TO_TICKS(100);
constexpr TickType_t REMOTE_POLL_DELAY = pdMS_TO_TICKS(2500);

static KageBridgeInfo s_info = {};
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_last_sequence = UINT32_MAX;
static uint32_t s_last_assistant_sequence = UINT32_MAX;
static bool s_started;

// The ESP32-S3 should never run the command poll and a voice upload over
// HTTPS at the same time. TLS handshakes/buffers are comparatively expensive,
// especially when Kage is on the iPhone hotspot through Tailscale Funnel.
//
// A pending voice upload has priority. The mutex guarantees that a GET already
// in flight finishes before /audio starts, and that no new GET starts until the
// upload has completed.
static std::atomic<bool> s_voice_network_pending{false};
static std::atomic<bool> s_touch_hold_requested{false};
static std::atomic<bool> s_hold_worker_active{false};
static SemaphoreHandle_t s_http_mutex;
static StaticSemaphore_t s_http_mutex_storage;
static portMUX_TYPE s_http_mutex_init_lock = portMUX_INITIALIZER_UNLOCKED;

static SemaphoreHandle_t http_mutex() {
    if (s_http_mutex) return s_http_mutex;

    portENTER_CRITICAL(&s_http_mutex_init_lock);
    if (!s_http_mutex) {
        s_http_mutex = xSemaphoreCreateMutexStatic(&s_http_mutex_storage);
    }
    portEXIT_CRITICAL(&s_http_mutex_init_lock);
    return s_http_mutex;
}

static TickType_t current_poll_delay() {
    return wifi_service_active_profile_index() == 0 ? LOCAL_POLL_DELAY : REMOTE_POLL_DELAY;
}

static void copy_text(char *destination, size_t size, const char *source) {
    if (!destination || size == 0) return;
    if (!source) {
        destination[0] = 0;
        return;
    }
    size_t length = std::strlen(source);
    if (length >= size) length = size - 1;
    std::memcpy(destination, source, length);
    destination[length] = 0;
}

struct HttpResponse {
    char body[384];
    size_t length;
};

static esp_err_t http_event(esp_http_client_event_t *event) {
    auto *response = static_cast<HttpResponse *>(event->user_data);
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

static bool request_latest(const char *url, bool remote, HttpResponse *response,
                           int *status, const char **error) {
    if (!url || !response || !status || !error) return false;

    *response = {};
    *status = 0;
    *error = "";

    esp_http_client_config_t config = {};
    config.url = url;
    config.timeout_ms = remote ? 6000 : 1800;
    config.event_handler = http_event;
    config.user_data = response;
    if (remote) {
        config.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        *error = "HTTP init failed";
        return false;
    }

    const char *api_key = wifi_service_api_key();
    if (api_key && api_key[0]) {
        esp_http_client_set_header(client, "X-Kage-Key", api_key);
    }

    const esp_err_t result = esp_http_client_perform(client);
    *status = result == ESP_OK ? esp_http_client_get_status_code(client) : 0;
    if (result != ESP_OK) {
        *error = esp_err_to_name(result);
    } else if (*status != 200) {
        *error = "HTTP status error";
    }

    esp_http_client_cleanup(client);
    return result == ESP_OK && *status == 200;
}

static void update_info(bool reachable, int status, uint32_t sequence,
                        const char *command, const char *error) {
    portENTER_CRITICAL(&s_lock);
    s_info.reachable = reachable;
    s_info.http_status = status;
    s_info.sequence = sequence;
    copy_text(s_info.command, sizeof(s_info.command), command ? command : "");
    copy_text(s_info.error, sizeof(s_info.error), error ? error : "");
    portEXIT_CRITICAL(&s_lock);
}

static void dispatch_command(const char *command) {
    if (!command) return;
    if (std::strcmp(command, "dance") != 0) robot_motion_stop_dance();
    int first = 0;
    int second = 0;
    if (std::sscanf(command, "drive:%d:%d", &first, &second) == 2) {
        robot_drive_analog(first, second);
        robot_eyes_set_drive_state(first, second);
        robot_eyes_set_behavior(ROBOT_BEHAVIOR_NEUTRAL, 100);
        return;
    }
    char behavior_name[32] = {};
    if (std::sscanf(command, "behavior:%31[^:]", behavior_name) == 1) {
        int behavior = ROBOT_BEHAVIOR_NEUTRAL;
        if (std::strcmp(behavior_name, "listening") == 0) behavior = ROBOT_BEHAVIOR_LISTENING;
        else if (std::strcmp(behavior_name, "thinking") == 0) behavior = ROBOT_BEHAVIOR_THINKING;
        else if (std::strcmp(behavior_name, "speaking") == 0) behavior = ROBOT_BEHAVIOR_SPEAKING;
        else if (std::strcmp(behavior_name, "affirm") == 0) behavior = ROBOT_BEHAVIOR_AFFIRM;
        else if (std::strcmp(behavior_name, "deny") == 0) behavior = ROBOT_BEHAVIOR_DENY;
        else if (std::strcmp(behavior_name, "amused") == 0) behavior = ROBOT_BEHAVIOR_AMUSED;
        else if (std::strcmp(behavior_name, "sarcastic") == 0) behavior = ROBOT_BEHAVIOR_SARCASTIC;
        else if (std::strcmp(behavior_name, "confused") == 0) behavior = ROBOT_BEHAVIOR_CONFUSED;
        else if (std::strcmp(behavior_name, "warning") == 0) behavior = ROBOT_BEHAVIOR_WARNING;
        else if (std::strcmp(behavior_name, "celebrate") == 0) behavior = ROBOT_BEHAVIOR_CELEBRATE;
        else if (std::strcmp(behavior_name, "happy") == 0) behavior = ROBOT_BEHAVIOR_HAPPY;
        else if (std::strcmp(behavior_name, "curious") == 0) behavior = ROBOT_BEHAVIOR_CURIOUS;
        else if (std::strcmp(behavior_name, "surprised") == 0) behavior = ROBOT_BEHAVIOR_SURPRISED;
        else if (std::strcmp(behavior_name, "worried") == 0) behavior = ROBOT_BEHAVIOR_WORRIED;
        else if (std::strcmp(behavior_name, "sad") == 0) behavior = ROBOT_BEHAVIOR_SAD;
        else if (std::strcmp(behavior_name, "satisfied") == 0) behavior = ROBOT_BEHAVIOR_SATISFIED;
        else if (std::strcmp(behavior_name, "gentle") == 0) behavior = ROBOT_BEHAVIOR_GENTLE;
        else if (std::strcmp(behavior_name, "explore") == 0) behavior = ROBOT_BEHAVIOR_EXPLORE;
        if (behavior != ROBOT_BEHAVIOR_NEUTRAL) {
            // Conversational cues affect the face only.  Moving a servo while
            // the assistant starts listening can disturb the robot's power and
            // Wi-Fi; physical motion remains reserved for explicit commands.
            robot_eyes_set_behavior(behavior, behavior == ROBOT_BEHAVIOR_EXPLORE ? 3000 : 1500);
        }
        return;
    }
    if (std::sscanf(command, "servo:%d:%d", &first, &second) == 2) {
        robot_motion_cancel_behavior();
        robot_servo_targets(first, second);
        robot_eyes_set_manual_look(first < 46 ? -1 : (first > 54 ? 1 : 0));
        return;
    }
    if (std::sscanf(command, "servo_pose:%d:%d", &first, &second) == 2) {
        robot_motion_cancel_behavior();
        robot_servo_pose(first, second);
        robot_eyes_set_manual_look(first);
        return;
    }
    if (std::sscanf(command, "servo_adjust:%d:%d", &first, &second) == 2) {
        robot_servo_adjust(first, second);
        return;
    }
    int third = 0;
    int fourth = 0;
    if (std::sscanf(command, "motion_config:%d:%d:%d:%d",
                    &first, &second, &third, &fourth) == 4) {
        (void)robot_motion_settings(first, second, third, fourth);
        return;
    }
    if (std::strcmp(command, "servo_cal_begin") == 0) {
        robot_servo_calibration_begin();
        return;
    }
    if (std::strcmp(command, "servo_cal_save") == 0) {
        (void)robot_servo_calibration_save();
        return;
    }
    if (std::strcmp(command, "idle") == 0) {
        robot_motion_stop();
        robot_eyes_remote_idle();
        robot_eyes_set_drive_state(0, 0);
    }
    else if (std::strcmp(command, "blink") == 0) robot_eyes_remote_blink();
    else if (std::strcmp(command, "sleep") == 0) robot_eyes_remote_sleep();
    else if (std::strcmp(command, "angry") == 0) robot_eyes_remote_angry();
    else if (std::strcmp(command, "dizzy") == 0) robot_eyes_remote_dizzy();
    else if (std::strcmp(command, "move_h") == 0) robot_motion_command(ROBOT_MOVE_HORIZONTAL);
    else if (std::strcmp(command, "move_v") == 0) robot_motion_command(ROBOT_MOVE_VERTICAL);
    else if (std::strcmp(command, "move_f") == 0) robot_motion_command(ROBOT_MOVE_FORWARD);
    else if (std::strcmp(command, "move_b") == 0) robot_motion_command(ROBOT_MOVE_BACKWARD);
    else if (std::strcmp(command, "drive_f") == 0) robot_drive_hold(ROBOT_DRIVE_FORWARD);
    else if (std::strcmp(command, "drive_b") == 0) robot_drive_hold(ROBOT_DRIVE_BACKWARD);
    else if (std::strcmp(command, "drive_l") == 0) robot_drive_hold(ROBOT_DRIVE_LEFT);
    else if (std::strcmp(command, "drive_r") == 0) robot_drive_hold(ROBOT_DRIVE_RIGHT);
    else if (std::strcmp(command, "motion_stop") == 0) {
        robot_motion_stop();
        robot_eyes_set_drive_state(0, 0);
        robot_eyes_set_manual_look(0);
    }
    else if (std::strcmp(command, "dance") == 0) {
        robot_motion_start_dance();
        robot_eyes_set_behavior(ROBOT_BEHAVIOR_CELEBRATE, 3000);
    }
    else if (std::strcmp(command, "explore") == 0) {
        robot_motion_behavior(ROBOT_BEHAVIOR_EXPLORE, 3000);
        robot_eyes_set_behavior(ROBOT_BEHAVIOR_EXPLORE, 3000);
    }
    else if (std::strcmp(command, "pan_l") == 0) {
        robot_servo_nudge(ROBOT_PAN_LEFT);
        robot_eyes_set_manual_look(-1);
    }
    else if (std::strcmp(command, "pan_r") == 0) {
        robot_servo_nudge(ROBOT_PAN_RIGHT);
        robot_eyes_set_manual_look(1);
    }
    else if (std::strcmp(command, "tilt_u") == 0) robot_servo_nudge(ROBOT_TILT_UP);
    else if (std::strcmp(command, "tilt_d") == 0) robot_servo_nudge(ROBOT_TILT_DOWN);
    else {
        ESP_LOGW("kage-bridge", "Unknown command: %s", command);
        event_log_add("Unknown command: %s", command);
    }
}

static void toggle_voice_task(void *) {
    if (wifi_service_active_profile_index() != 0) {
        event_log_add("Voice toggle: local PC unavailable");
        vTaskDelete(nullptr);
        return;
    }
    SemaphoreHandle_t mutex = http_mutex();
    if (!mutex || xSemaphoreTake(mutex, pdMS_TO_TICKS(1500)) != pdTRUE) {
        event_log_add("Voice toggle: network busy");
        vTaskDelete(nullptr);
        return;
    }
    esp_http_client_config_t config = {};
    config.url = LOCAL_TOGGLE_URL;
    config.timeout_ms = 2500;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        xSemaphoreGive(mutex);
        event_log_add("Voice toggle: HTTP init failed");
        vTaskDelete(nullptr);
        return;
    }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    const char *api_key = wifi_service_api_key();
    if (api_key && api_key[0]) esp_http_client_set_header(client, "X-Kage-Key", api_key);
    const esp_err_t result = esp_http_client_perform(client);
    const int status = result == ESP_OK ? esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);
    xSemaphoreGive(mutex);
    if (result == ESP_OK && status == 200) event_log_add("Voice toggled from touch");
    else event_log_add("Voice toggle failed: %d", status);
    vTaskDelete(nullptr);
}

static void interrupt_voice_task(void *) {
    if (wifi_service_active_profile_index() != 0) { vTaskDelete(nullptr); return; }
    SemaphoreHandle_t mutex = http_mutex();
    if (!mutex || xSemaphoreTake(mutex, pdMS_TO_TICKS(1500)) != pdTRUE) { vTaskDelete(nullptr); return; }
    esp_http_client_config_t config = {};
    config.url = LOCAL_INTERRUPT_URL;
    config.timeout_ms = 2500;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        const char *api_key = wifi_service_api_key();
        if (api_key && api_key[0]) esp_http_client_set_header(client, "X-Kage-Key", api_key);
        esp_http_client_perform(client);
        esp_http_client_cleanup(client);
    }
    xSemaphoreGive(mutex);
    vTaskDelete(nullptr);
}

static void wake_voice_task(void *) {
    if (wifi_service_active_profile_index() != 0) {
        event_log_add("Voice wake: local PC unavailable");
        vTaskDelete(nullptr);
        return;
    }
    SemaphoreHandle_t mutex = http_mutex();
    if (!mutex || xSemaphoreTake(mutex, pdMS_TO_TICKS(1500)) != pdTRUE) {
        event_log_add("Voice wake: network busy");
        vTaskDelete(nullptr);
        return;
    }
    esp_http_client_config_t config = {};
    config.url = LOCAL_WAKE_URL;
    config.timeout_ms = 2500;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t result = ESP_FAIL;
    int status = 0;
    if (client) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        const char *api_key = wifi_service_api_key();
        if (api_key && api_key[0]) esp_http_client_set_header(client, "X-Kage-Key", api_key);
        result = esp_http_client_perform(client);
        status = result == ESP_OK ? esp_http_client_get_status_code(client) : 0;
        esp_http_client_cleanup(client);
    }
    xSemaphoreGive(mutex);
    if (result == ESP_OK && status == 200) event_log_add("Voice wake from double tap");
    else event_log_add("Voice wake failed: %d", status);
    vTaskDelete(nullptr);
}

static void sleep_voice_task(void *) {
    if (wifi_service_active_profile_index() != 0) {
        event_log_add("Voice sleep: local PC unavailable");
        vTaskDelete(nullptr);
        return;
    }
    SemaphoreHandle_t mutex = http_mutex();
    if (!mutex || xSemaphoreTake(mutex, pdMS_TO_TICKS(1500)) != pdTRUE) {
        event_log_add("Voice sleep: network busy");
        vTaskDelete(nullptr);
        return;
    }
    esp_http_client_config_t config = {};
    config.url = LOCAL_SLEEP_URL;
    config.timeout_ms = 2500;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t result = ESP_FAIL;
    int status = 0;
    if (client) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        const char *api_key = wifi_service_api_key();
        if (api_key && api_key[0]) esp_http_client_set_header(client, "X-Kage-Key", api_key);
        result = esp_http_client_perform(client);
        status = result == ESP_OK ? esp_http_client_get_status_code(client) : 0;
        esp_http_client_cleanup(client);
    }
    xSemaphoreGive(mutex);
    if (result == ESP_OK && status == 200) event_log_add("Voice sleep from double tap");
    else event_log_add("Voice sleep failed: %d", status);
    vTaskDelete(nullptr);
}

static void send_hold_request(const char *url) {
    if (wifi_service_active_profile_index() != 0) {
        event_log_add("Voice hold: local PC unavailable");
        return;
    }
    SemaphoreHandle_t mutex = http_mutex();
    if (!mutex || xSemaphoreTake(mutex, pdMS_TO_TICKS(1500)) != pdTRUE) {
        event_log_add("Voice hold: network busy");
        return;
    }
    esp_http_client_config_t config = {};
    config.url = url;
    config.timeout_ms = 2500;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t result = ESP_FAIL;
    int status = 0;
    if (client) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        const char *api_key = wifi_service_api_key();
        if (api_key && api_key[0]) esp_http_client_set_header(client, "X-Kage-Key", api_key);
        result = esp_http_client_perform(client);
        status = result == ESP_OK ? esp_http_client_get_status_code(client) : 0;
        esp_http_client_cleanup(client);
    }
    xSemaphoreGive(mutex);
    if (result != ESP_OK || status != 200) event_log_add("Voice hold failed: %d", status);
}

static void hold_voice_task(void *) {
    // One worker preserves request order even if the user releases immediately.
    while (true) {
        send_hold_request(LOCAL_HOLD_START_URL);
        while (s_touch_hold_requested.load(std::memory_order_acquire)) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        send_hold_request(LOCAL_HOLD_STOP_URL);
        s_hold_worker_active.store(false, std::memory_order_release);
        if (!s_touch_hold_requested.load(std::memory_order_acquire)) break;
        bool expected = false;
        if (!s_hold_worker_active.compare_exchange_strong(expected, true)) break;
    }
    vTaskDelete(nullptr);
}

static void dispatch_assistant_state(const char *assistant_state) {
    if (!assistant_state) return;
    if (std::strcmp(assistant_state, "idle") == 0) robot_eyes_assistant_idle();
    else if (std::strcmp(assistant_state, "listening") == 0) robot_eyes_assistant_listening();
    else if (std::strcmp(assistant_state, "thinking") == 0) robot_eyes_assistant_thinking();
    else if (std::strcmp(assistant_state, "speaking") == 0) robot_eyes_assistant_speaking();
    else if (std::strcmp(assistant_state, "error") == 0) robot_eyes_assistant_error();
    else if (std::strcmp(assistant_state, "offline") == 0) robot_eyes_assistant_offline();
}

static void apply_assistant_state(const char *assistant_state, uint32_t sequence) {
    if (!assistant_state || !assistant_state[0] || sequence == s_last_assistant_sequence) return;
    s_last_assistant_sequence = sequence;
    ESP_LOGI("kage-bridge", "Assistant state #%lu: %s",
             static_cast<unsigned long>(sequence), assistant_state);
    event_log_add("Assistant: %s", assistant_state);
    dispatch_assistant_state(assistant_state);
}

void apply_command_impl(const char *command, uint32_t sequence) {
    if (!command || !command[0] || std::strcmp(command, "none") == 0) return;

    bool is_new = false;
    portENTER_CRITICAL(&s_lock);
    if (sequence != s_last_sequence) {
        s_last_sequence = sequence;
        is_new = true;
    }
    portEXIT_CRITICAL(&s_lock);

    if (!is_new) return;
    ESP_LOGI("kage-bridge", "Immediate command #%lu: %s",
             static_cast<unsigned long>(sequence), command);
    event_log_add("Immediate command #%lu: %s",
                  static_cast<unsigned long>(sequence), command);
    dispatch_command(command);
}

static void bridge_task(void *) {
    bool was_reachable = false;

    for (;;) {
        if (!wifi_service_connected()) {
            if (was_reachable) {
                event_log_add("Backend offline: Wi-Fi lost");
                robot_motion_stop_dance();
                robot_eyes_set_drive_state(0, 0);
                was_reachable = false;
            }
            update_info(false, 0, s_last_sequence == UINT32_MAX ? 0 : s_last_sequence,
                        "", "Wi-Fi disconnected");
            vTaskDelay(current_poll_delay());
            continue;
        }

        HttpResponse response = {};
        int status = 0;
        const char *request_error = "";
        const bool on_primary_network = wifi_service_active_profile_index() == 0;

        // Give a pending voice upload priority over command polling.
        if (s_voice_network_pending.load(std::memory_order_acquire)) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        SemaphoreHandle_t mutex = http_mutex();
        if (!mutex || xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // Voice may have become pending while this task was waiting for the
        // mutex. Yield immediately instead of starting another GET.
        if (s_voice_network_pending.load(std::memory_order_acquire)) {
            xSemaphoreGive(mutex);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        bool request_ok = false;
        if (on_primary_network) {
            request_ok = request_latest(
                LOCAL_BACKEND_URL, false, &response, &status, &request_error);
            if (!request_ok && wifi_service_has_api_key()) {
                request_ok = request_latest(
                    REMOTE_BACKEND_URL, true, &response, &status, &request_error);
            }
        } else if (wifi_service_has_api_key()) {
            request_ok = request_latest(
                REMOTE_BACKEND_URL, true, &response, &status, &request_error);
        } else {
            request_error = "Remote key missing";
        }

        xSemaphoreGive(mutex);

        if (!request_ok) {
            if (was_reachable) {
                event_log_add("Backend offline: %s", request_error);
                robot_motion_stop_dance();
                robot_eyes_set_drive_state(0, 0);
            }
            was_reachable = false;
            update_info(false, status, s_last_sequence == UINT32_MAX ? 0 : s_last_sequence,
                        "", request_error);
            vTaskDelay(current_poll_delay());
            continue;
        }

        cJSON *root = cJSON_Parse(response.body);
        cJSON *command_item = root ? cJSON_GetObjectItem(root, "command") : nullptr;
        cJSON *sequence_item = root ? cJSON_GetObjectItem(root, "sequence") : nullptr;
        cJSON *assistant_state_item = root ? cJSON_GetObjectItem(root, "assistant_state") : nullptr;
        cJSON *assistant_sequence_item = root ? cJSON_GetObjectItem(root, "assistant_sequence") : nullptr;
        cJSON *voice_active_item = root ? cJSON_GetObjectItem(root, "voice_active") : nullptr;
        cJSON *mouth_level_item = root ? cJSON_GetObjectItem(root, "mouth_level") : nullptr;
        if (!root || !cJSON_IsString(command_item) || !cJSON_IsNumber(sequence_item)) {
            if (root) cJSON_Delete(root);
            if (was_reachable) event_log_add("Backend response invalid");
            was_reachable = false;
            update_info(false, status, s_last_sequence == UINT32_MAX ? 0 : s_last_sequence,
                        "", "Invalid JSON");
            vTaskDelay(current_poll_delay());
            continue;
        }

        const char *command = command_item->valuestring;
        const uint32_t sequence = static_cast<uint32_t>(sequence_item->valuedouble);

        if (!was_reachable) {
            ESP_LOGI("kage-bridge", "M920q backend reachable");
            event_log_add("Backend online");
        }
        was_reachable = true;
        update_info(true, status, sequence, command, "");

        apply_command_impl(command, sequence);
        if (cJSON_IsString(assistant_state_item) && cJSON_IsNumber(assistant_sequence_item)) {
            apply_assistant_state(assistant_state_item->valuestring,
                                  static_cast<uint32_t>(assistant_sequence_item->valuedouble));
        }
        robot_eyes_set_voice_active(cJSON_IsTrue(voice_active_item));
        if (cJSON_IsNumber(mouth_level_item)) {
            robot_eyes_set_mouth_level(static_cast<int>(mouth_level_item->valuedouble));
        }

        cJSON_Delete(root);
        vTaskDelay(current_poll_delay());
    }
}
}  // namespace

void kage_bridge_apply_command(const char *command, uint32_t sequence) {
    apply_command_impl(command, sequence);
}

void kage_bridge_begin(void) {
    if (s_started) return;
    s_started = true;
    (void)http_mutex();
    event_log_add("Command bridge starting");
    xTaskCreate(bridge_task, "kage_bridge", 6144, nullptr, 4, nullptr);
}

void kage_bridge_toggle_voice(void) {
    if (!s_started) return;
    xTaskCreate(toggle_voice_task, "kage_voice_toggle", 4096, nullptr, 4, nullptr);
}

void kage_bridge_wake_voice(void) {
    if (!s_started) return;
    xTaskCreate(wake_voice_task, "kage_wake", 4096, nullptr, 4, nullptr);
}

void kage_bridge_sleep_voice(void) {
    if (!s_started) return;
    xTaskCreate(sleep_voice_task, "kage_sleep", 4096, nullptr, 4, nullptr);
}

void kage_bridge_interrupt_voice(void) {
    xTaskCreate(interrupt_voice_task, "kage_voice_interrupt", 4096, nullptr, 4, nullptr);
}

void kage_bridge_hold_start(void) {
    if (!s_started) return;
    s_touch_hold_requested.store(true, std::memory_order_release);
    bool expected = false;
    if (s_hold_worker_active.compare_exchange_strong(expected, true)) {
        if (xTaskCreate(hold_voice_task, "kage_hold", 4096, nullptr, 4, nullptr) != pdPASS) {
            s_hold_worker_active.store(false, std::memory_order_release);
            s_touch_hold_requested.store(false, std::memory_order_release);
            event_log_add("Voice hold: task unavailable");
        }
    }
}

void kage_bridge_hold_stop(void) {
    s_touch_hold_requested.store(false, std::memory_order_release);
}

bool kage_bridge_voice_upload_begin(uint32_t timeout_ms) {
    s_voice_network_pending.store(true, std::memory_order_release);

    SemaphoreHandle_t mutex = http_mutex();
    if (!mutex ||
        xSemaphoreTake(mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        s_voice_network_pending.store(false, std::memory_order_release);
        return false;
    }

    return true;
}

void kage_bridge_voice_upload_end(void) {
    SemaphoreHandle_t mutex = http_mutex();
    s_voice_network_pending.store(false, std::memory_order_release);
    if (mutex) {
        xSemaphoreGive(mutex);
    }
}

void kage_bridge_get_info(KageBridgeInfo *info) {
    if (!info) return;
    portENTER_CRITICAL(&s_lock);
    *info = s_info;
    portEXIT_CRITICAL(&s_lock);
}
