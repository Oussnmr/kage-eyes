#include "kage_bridge.h"

#include <cstring>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "eyes/robot_eyes.h"
#include "event_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_service.h"

namespace {
constexpr const char *BACKEND_URL = "http://192.168.129.157:8000/command/latest";
constexpr TickType_t POLL_DELAY = pdMS_TO_TICKS(500);

static KageBridgeInfo s_info = {};
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_last_sequence = UINT32_MAX;
static bool s_started;

struct HttpResponse {
    char body[192];
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

static void update_info(bool reachable, int status, uint32_t sequence,
                        const char *command, const char *error) {
    portENTER_CRITICAL(&s_lock);
    s_info.reachable = reachable;
    s_info.http_status = status;
    s_info.sequence = sequence;
    std::strncpy(s_info.command, command ? command : "", sizeof(s_info.command) - 1);
    s_info.command[sizeof(s_info.command) - 1] = 0;
    std::strncpy(s_info.error, error ? error : "", sizeof(s_info.error) - 1);
    s_info.error[sizeof(s_info.error) - 1] = 0;
    portEXIT_CRITICAL(&s_lock);
}

static void dispatch_command(const char *command) {
    if (!command) return;
    if (std::strcmp(command, "idle") == 0) robot_eyes_remote_idle();
    else if (std::strcmp(command, "blink") == 0) robot_eyes_remote_blink();
    else if (std::strcmp(command, "sleep") == 0) robot_eyes_remote_sleep();
    else if (std::strcmp(command, "angry") == 0) robot_eyes_remote_angry();
    else if (std::strcmp(command, "dizzy") == 0) robot_eyes_remote_dizzy();
    else {
        ESP_LOGW("kage-bridge", "Unknown command: %s", command);
        event_log_add("Unknown command: %s", command);
    }
}

static void bridge_task(void *) {
    bool was_reachable = false;

    for (;;) {
        if (!wifi_service_connected()) {
            if (was_reachable) {
                event_log_add("Backend offline: Wi-Fi lost");
                was_reachable = false;
            }
            update_info(false, 0, s_last_sequence == UINT32_MAX ? 0 : s_last_sequence,
                        "", "Wi-Fi disconnected");
            vTaskDelay(POLL_DELAY);
            continue;
        }

        HttpResponse response = {};
        esp_http_client_config_t config = {};
        config.url = BACKEND_URL;
        config.timeout_ms = 1800;
        config.event_handler = http_event;
        config.user_data = &response;

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            update_info(false, 0, s_last_sequence == UINT32_MAX ? 0 : s_last_sequence,
                        "", "HTTP init failed");
            vTaskDelay(POLL_DELAY);
            continue;
        }

        const esp_err_t result = esp_http_client_perform(client);
        const int status = result == ESP_OK ? esp_http_client_get_status_code(client) : 0;
        esp_http_client_cleanup(client);

        if (result != ESP_OK || status != 200) {
            const char *error = result == ESP_OK ? "HTTP status error" : esp_err_to_name(result);
            if (was_reachable) event_log_add("Backend offline: %s", error);
            was_reachable = false;
            update_info(false, status, s_last_sequence == UINT32_MAX ? 0 : s_last_sequence,
                        "", error);
            vTaskDelay(POLL_DELAY);
            continue;
        }

        cJSON *root = cJSON_Parse(response.body);
        cJSON *command_item = root ? cJSON_GetObjectItem(root, "command") : nullptr;
        cJSON *sequence_item = root ? cJSON_GetObjectItem(root, "sequence") : nullptr;
        if (!root || !cJSON_IsString(command_item) || !cJSON_IsNumber(sequence_item)) {
            if (root) cJSON_Delete(root);
            if (was_reachable) event_log_add("Backend response invalid");
            was_reachable = false;
            update_info(false, status, s_last_sequence == UINT32_MAX ? 0 : s_last_sequence,
                        "", "Invalid JSON");
            vTaskDelay(POLL_DELAY);
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

        if (sequence != s_last_sequence) {
            s_last_sequence = sequence;
            ESP_LOGI("kage-bridge", "Command #%lu: %s",
                     static_cast<unsigned long>(sequence), command);
            event_log_add("Command #%lu: %s",
                          static_cast<unsigned long>(sequence), command);
            dispatch_command(command);
        }

        cJSON_Delete(root);
        vTaskDelay(POLL_DELAY);
    }
}
}  // namespace

void kage_bridge_begin(void) {
    if (s_started) return;
    s_started = true;
    event_log_add("Command bridge starting");
    xTaskCreate(bridge_task, "kage_bridge", 6144, nullptr, 4, nullptr);
}

void kage_bridge_get_info(KageBridgeInfo *info) {
    if (!info) return;
    portENTER_CRITICAL(&s_lock);
    *info = s_info;
    portEXIT_CRITICAL(&s_lock);
}
