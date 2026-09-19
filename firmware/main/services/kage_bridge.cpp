#include "kage_bridge.h"

#include <cstring>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "eyes/robot_eyes.h"
#include "event_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_service.h"

namespace {
constexpr const char *LOCAL_BACKEND_URL = "http://192.168.129.157:8000/command/latest";
constexpr const char *REMOTE_BACKEND_URL = "https://m920q.tailbf4c85.ts.net:8443/command/latest";
constexpr TickType_t POLL_DELAY = pdMS_TO_TICKS(500);

static KageBridgeInfo s_info = {};
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_last_sequence = UINT32_MAX;
static bool s_started;

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
        int status = 0;
        const char *request_error = "";
        const bool on_primary_network = wifi_service_active_profile_index() == 0;

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

        if (!request_ok) {
            if (was_reachable) event_log_add("Backend offline: %s", request_error);
            was_reachable = false;
            update_info(false, status, s_last_sequence == UINT32_MAX ? 0 : s_last_sequence,
                        "", request_error);
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
