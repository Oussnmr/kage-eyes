/* Improv Wi-Fi Serial service.
 *
 * Kage keeps up to two Wi-Fi profiles in its own NVS namespace. The first
 * profile is imported from the existing ESP-IDF station configuration on the
 * first boot after upgrading, so the current home network is preserved.
 *
 * When Kage is already online and Improv receives credentials for a different
 * SSID, they are stored as the second profile without interrupting the current
 * connection. If the active network disappears, Kage automatically alternates
 * between the stored profiles after a few failed reconnects.
 *
 * No credentials are compiled into the firmware or committed to the repo.
 */
#include "wifi_service.h"

#include <cstdio>
#include <cstring>

#include "driver/usb_serial_jtag.h"
#include "esp_event.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_netif_ip_addr.h"
#include "esp_wifi.h"
#include "event_log.h"
#include "audio/mic_meter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

namespace {
constexpr uint8_t TYPE_STATE = 0x01;
constexpr uint8_t TYPE_ERROR = 0x02;
constexpr uint8_t TYPE_RPC = 0x03;
constexpr uint8_t TYPE_RPC_RESPONSE = 0x04;

constexpr uint8_t RPC_WIFI_SETTINGS = 0x01;
constexpr uint8_t RPC_GET_STATE = 0x02;
constexpr uint8_t RPC_DEVICE_INFO = 0x03;

constexpr uint8_t STATE_AUTHORIZED = 0x02;
constexpr uint8_t STATE_PROVISIONING = 0x03;
constexpr uint8_t STATE_PROVISIONED = 0x04;

constexpr char NVS_NAMESPACE[] = "kage_wifi";
constexpr char KEY_P0_SSID[] = "p0_ssid";
constexpr char KEY_P0_PASS[] = "p0_pass";
constexpr char KEY_P1_SSID[] = "p1_ssid";
constexpr char KEY_P1_PASS[] = "p1_pass";
constexpr int PROFILE_COUNT = 2;
constexpr int FAILURES_BEFORE_SWITCH = 3;
constexpr char KEY_API_KEY[] = "api_key";
constexpr size_t API_KEY_HEX_LENGTH = 64;

struct WifiProfile {
    bool valid;
    char ssid[33];
    char password[65];
};

static WifiProfile profiles[PROFILE_COUNT] = {};
static int active_profile = -1;
static int reconnect_failures = 0;
static char api_key[API_KEY_HEX_LENGTH + 1] = {};
static char key_line[96] = {};
static size_t key_line_len = 0;
static bool key_line_active = false;
static bool sntp_started = false;
static httpd_handle_t status_server = nullptr;

static bool connected;
static bool configured;
static bool provisioning_request;
static int32_t last_disconnect_reason;
static char ssid[33] = {};
static char ip_address[16] = {};
static char gateway[16] = {};
static char subnet_mask[16] = {};
static uint8_t input[320];
static size_t input_len;
static portMUX_TYPE info_lock = portMUX_INITIALIZER_UNLOCKED;

static const char *mic_state_name(MicMeterState state) {
    switch (state) {
        case MicMeterState::Off: return "off";
        case MicMeterState::Starting: return "starting";
        case MicMeterState::Listening: return "listening";
        case MicMeterState::Processing: return "processing";
        case MicMeterState::Error: return "error";
    }
    return "unknown";
}

static esp_err_t status_get(httpd_req_t *request) {
    const esp_app_desc_t *app = esp_app_get_description();
    bool online = false;
    char address[sizeof(ip_address)] = {};
    portENTER_CRITICAL(&info_lock);
    online = connected;
    std::snprintf(address, sizeof(address), "%s", ip_address);
    portEXIT_CRITICAL(&info_lock);
    char response[256] = {};
    std::snprintf(response, sizeof(response),
                  "{\"device\":\"Kage Eyes\",\"firmware\":\"%s\",\"wifi_connected\":%s,\"ip\":\"%s\",\"microphone\":\"%s\"}",
                  app && app->version[0] ? app->version : "unknown",
                  online ? "true" : "false", address,
                  mic_state_name(mic_meter_state()));
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, response);
}

static void start_status_server() {
    if (status_server) return;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 3072;
    config.max_open_sockets = 1;
    config.max_uri_handlers = 1;
    config.backlog_conn = 1;
    if (httpd_start(&status_server, &config) != ESP_OK) {
        status_server = nullptr;
        ESP_LOGW("kage-wifi", "Status server failed to start");
        return;
    }
    httpd_uri_t status = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = status_get,
    };
    if (httpd_register_uri_handler(status_server, &status) != ESP_OK) {
        httpd_stop(status_server);
        status_server = nullptr;
        ESP_LOGW("kage-wifi", "Status endpoint registration failed");
        return;
    }
    ESP_LOGI("kage-wifi", "Status endpoint ready at /status");
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

static const char *ssid_key(int index) {
    return index == 0 ? KEY_P0_SSID : KEY_P1_SSID;
}

static const char *pass_key(int index) {
    return index == 0 ? KEY_P0_PASS : KEY_P1_PASS;
}

static bool load_nvs_string(nvs_handle_t handle, const char *key,
                            char *destination, size_t size) {
    size_t required = size;
    const esp_err_t result = nvs_get_str(handle, key, destination, &required);
    if (result != ESP_OK || destination[0] == 0) {
        if (size) destination[0] = 0;
        return false;
    }
    destination[size - 1] = 0;
    return true;
}

static bool is_hex_key(const char *value) {
    if (!value || std::strlen(value) != API_KEY_HEX_LENGTH) return false;
    for (size_t i = 0; i < API_KEY_HEX_LENGTH; ++i) {
        const char ch = value[i];
        const bool digit = ch >= '0' && ch <= '9';
        const bool lower = ch >= 'a' && ch <= 'f';
        const bool upper = ch >= 'A' && ch <= 'F';
        if (!digit && !lower && !upper) return false;
    }
    return true;
}

static bool save_api_key_to_nvs(const char *value) {
    if (!is_hex_key(value)) return false;

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;

    esp_err_t result = nvs_set_str(handle, KEY_API_KEY, value);
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);

    if (result == ESP_OK) {
        copy_text(api_key, sizeof(api_key), value);
        event_log_add("Remote access key saved");
        ESP_LOGI("kage-wifi", "Remote access key saved in NVS");
        return true;
    }
    return false;
}

static void load_api_key_from_nvs() {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;

    size_t required = sizeof(api_key);
    const esp_err_t result = nvs_get_str(handle, KEY_API_KEY, api_key, &required);
    nvs_close(handle);

    if (result != ESP_OK || !is_hex_key(api_key)) {
        api_key[0] = 0;
        return;
    }

    ESP_LOGI("kage-wifi", "Remote access key found in NVS");
    event_log_add("Remote access key ready");
}

static void start_sntp_if_needed() {
    if (sntp_started) return;
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    const esp_err_t result = esp_netif_sntp_init(&config);
    if (result == ESP_OK) {
        sntp_started = true;
        ESP_LOGI("kage-wifi", "SNTP time sync started");
    } else if (result != ESP_ERR_INVALID_STATE) {
        ESP_LOGW("kage-wifi", "SNTP init failed: %s", esp_err_to_name(result));
    }
}

static void send_serial_text(const char *text) {
    if (!text) return;
    usb_serial_jtag_write_bytes(
        reinterpret_cast<const uint8_t *>(text),
        std::strlen(text),
        pdMS_TO_TICKS(100));
}

static void finish_key_line() {
    key_line[key_line_len] = 0;
    constexpr char prefix[] = "KAGEKEY:";
    const size_t prefix_len = sizeof(prefix) - 1;

    if (std::strncmp(key_line, prefix, prefix_len) == 0 &&
        save_api_key_to_nvs(key_line + prefix_len)) {
        send_serial_text("KAGEKEY:OK\n");
    } else {
        send_serial_text("KAGEKEY:ERROR\n");
    }

    key_line_active = false;
    key_line_len = 0;
    key_line[0] = 0;
}

static bool save_profile_to_nvs(int index) {
    if (index < 0 || index >= PROFILE_COUNT || !profiles[index].valid) {
        return false;
    }

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }

    esp_err_t result = nvs_set_str(handle, ssid_key(index), profiles[index].ssid);
    if (result == ESP_OK) {
        result = nvs_set_str(handle, pass_key(index), profiles[index].password);
    }
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result == ESP_OK;
}

static void load_profiles_from_nvs() {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }

    for (int i = 0; i < PROFILE_COUNT; ++i) {
        profiles[i] = {};
        const bool have_ssid =
            load_nvs_string(handle, ssid_key(i), profiles[i].ssid,
                            sizeof(profiles[i].ssid));
        if (have_ssid) {
            size_t required = sizeof(profiles[i].password);
            const esp_err_t pass_result =
                nvs_get_str(handle, pass_key(i), profiles[i].password, &required);
            if (pass_result != ESP_OK) profiles[i].password[0] = 0;
            profiles[i].password[sizeof(profiles[i].password) - 1] = 0;
            profiles[i].valid = true;
        }
    }

    nvs_close(handle);
}

static void import_legacy_profile_if_needed(const wifi_config_t &legacy) {
    if (profiles[0].valid || legacy.sta.ssid[0] == 0) return;

    profiles[0] = {};
    copy_text(profiles[0].ssid, sizeof(profiles[0].ssid),
              reinterpret_cast<const char *>(legacy.sta.ssid));
    copy_text(profiles[0].password, sizeof(profiles[0].password),
              reinterpret_cast<const char *>(legacy.sta.password));
    profiles[0].valid = true;

    if (save_profile_to_nvs(0)) {
        ESP_LOGI("kage-wifi", "Imported existing Wi-Fi as profile 1: %s",
                 profiles[0].ssid);
        event_log_add("Saved Wi-Fi profile 1: %s", profiles[0].ssid);
    }
}

static int first_valid_profile() {
    for (int i = 0; i < PROFILE_COUNT; ++i) {
        if (profiles[i].valid) return i;
    }
    return -1;
}

static int other_valid_profile(int current) {
    for (int i = 0; i < PROFILE_COUNT; ++i) {
        if (i != current && profiles[i].valid) return i;
    }
    return -1;
}

static int profile_for_ssid(const char *network_ssid) {
    if (!network_ssid) return -1;
    for (int i = 0; i < PROFILE_COUNT; ++i) {
        if (profiles[i].valid &&
            std::strcmp(profiles[i].ssid, network_ssid) == 0) {
            return i;
        }
    }
    return -1;
}

static int slot_for_new_profile() {
    for (int i = 0; i < PROFILE_COUNT; ++i) {
        if (!profiles[i].valid) return i;
    }
    const int other = other_valid_profile(active_profile);
    return other >= 0 ? other : 1;
}

static esp_err_t configure_profile_in_ram(int index) {
    if (index < 0 || index >= PROFILE_COUNT || !profiles[index].valid) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t config = {};
    copy_text(reinterpret_cast<char *>(config.sta.ssid),
              sizeof(config.sta.ssid), profiles[index].ssid);
    copy_text(reinterpret_cast<char *>(config.sta.password),
              sizeof(config.sta.password), profiles[index].password);

    const esp_err_t result = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (result != ESP_OK) return result;

    active_profile = index;

    portENTER_CRITICAL(&info_lock);
    copy_text(ssid, sizeof(ssid), profiles[index].ssid);
    configured = true;
    portEXIT_CRITICAL(&info_lock);

    ESP_LOGI("kage-wifi", "Selected Wi-Fi profile %d: %s",
             index + 1, profiles[index].ssid);
    return ESP_OK;
}

static esp_err_t connect_profile(int index) {
    const esp_err_t configured_result = configure_profile_in_ram(index);
    if (configured_result != ESP_OK) return configured_result;

    esp_wifi_disconnect();
    return esp_wifi_connect();
}

static void clear_runtime_connection_info() {
    portENTER_CRITICAL(&info_lock);
    connected = false;
    ip_address[0] = 0;
    gateway[0] = 0;
    subnet_mask[0] = 0;
    portEXIT_CRITICAL(&info_lock);
}

static void send_packet(uint8_t type, const uint8_t *data, size_t length) {
    if (length > 255) return;
    uint8_t packet[270] = {'I','M','P','R','O','V',1,type,static_cast<uint8_t>(length)};
    if (length) memcpy(packet + 9, data, length);
    size_t packet_len = 9 + length;
    uint8_t checksum = 0;
    for (size_t i = 0; i < packet_len; ++i) checksum += packet[i];
    packet[packet_len++] = checksum;
    usb_serial_jtag_write_bytes(packet, packet_len, pdMS_TO_TICKS(50));
}

static void send_state(uint8_t state) {
    send_packet(TYPE_STATE, &state, 1);
}

static void send_error(uint8_t error) {
    send_packet(TYPE_ERROR, &error, 1);
}

static void send_empty_rpc_response(uint8_t command) {
    const uint8_t data[2] = {command, 0};
    send_packet(TYPE_RPC_RESPONSE, data, sizeof(data));
}

static void send_device_info(void) {
    uint8_t data[240] = {RPC_DEVICE_INFO, 0};
    size_t pos = 2;
    const char *strings[] = {"Kage Eyes", "phase-1", "esp32-s3", "Kage Eyes"};
    for (const char *text : strings) {
        const size_t len = strlen(text);
        if (len > 255 || pos + len + 1 > sizeof(data)) return;
        data[pos++] = static_cast<uint8_t>(len);
        memcpy(data + pos, text, len);
        pos += len;
    }
    data[1] = static_cast<uint8_t>(pos - 2);
    send_packet(TYPE_RPC_RESPONSE, data, pos);
}

static void apply_wifi(const uint8_t *data, size_t length) {
    if (length < 4 || data[0] != RPC_WIFI_SETTINGS || data[1] != length - 2) {
        send_error(0x01);
        return;
    }

    const size_t ssid_len = data[2];
    if (ssid_len == 0 || ssid_len > 32 || 3 + ssid_len >= length) {
        send_error(0x01);
        return;
    }

    const size_t pass_len = data[3 + ssid_len];
    if (pass_len > 64 || 4 + ssid_len + pass_len != length) {
        send_error(0x01);
        return;
    }

    char incoming_ssid[33] = {};
    char incoming_password[65] = {};
    std::memcpy(incoming_ssid, data + 3, ssid_len);
    std::memcpy(incoming_password, data + 4 + ssid_len, pass_len);

    int target = profile_for_ssid(incoming_ssid);
    if (target < 0) target = slot_for_new_profile();

    profiles[target] = {};
    copy_text(profiles[target].ssid, sizeof(profiles[target].ssid), incoming_ssid);
    copy_text(profiles[target].password, sizeof(profiles[target].password),
              incoming_password);
    profiles[target].valid = true;

    if (!save_profile_to_nvs(target)) {
        send_error(0xFF);
        return;
    }

    ESP_LOGI("kage-wifi", "Stored Wi-Fi profile %d: %s",
             target + 1, profiles[target].ssid);
    event_log_add("Saved Wi-Fi profile %d: %s",
                  target + 1, profiles[target].ssid);

    send_error(0x00);
    send_state(STATE_PROVISIONING);

    /* If another profile is already online, keep it online. The new network is
       now ready for automatic fallback later. */
    if (connected && target != active_profile) {
        send_state(STATE_PROVISIONED);
        send_empty_rpc_response(RPC_WIFI_SETTINGS);
        return;
    }

    provisioning_request = true;
    reconnect_failures = 0;
    clear_runtime_connection_info();

    if (connect_profile(target) != ESP_OK) {
        provisioning_request = false;
        send_error(0xFF);
    }
}

static void handle_rpc(const uint8_t *data, size_t length) {
    if (length < 2 || data[1] != length - 2) {
        send_error(0x01);
        return;
    }

    send_error(0x00);
    const uint8_t command = data[0];

    if (command == RPC_WIFI_SETTINGS) {
        apply_wifi(data, length);
    } else if (command == RPC_GET_STATE) {
        const uint8_t state =
            connected ? STATE_PROVISIONED : (configured ? STATE_PROVISIONING : STATE_AUTHORIZED);
        send_state(state);
        if (connected) send_empty_rpc_response(RPC_WIFI_SETTINGS);
    } else if (command == RPC_DEVICE_INFO) {
        send_device_info();
    } else {
        send_error(0x02);
    }
}

static void serial_task(void *) {
    usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    config.rx_buffer_size = 512;
    config.tx_buffer_size = 512;

    const esp_err_t installed = usb_serial_jtag_driver_install(&config);
    if (installed != ESP_OK && installed != ESP_ERR_INVALID_STATE) {
        ESP_LOGW("improv", "USB serial unavailable: %s", esp_err_to_name(installed));
    }

    for (;;) {
        uint8_t bytes[64];
        const int count = usb_serial_jtag_read_bytes(bytes, sizeof(bytes), pdMS_TO_TICKS(50));

        for (int index = 0; index < count; ++index) {
            const uint8_t byte = bytes[index];
            static const uint8_t header[] = {'I','M','P','R','O','V'};

            /* A tiny local-only provisioning command for the remote API key.
               Send exactly: KAGEKEY:<64 hex characters> followed by newline.
               It is consumed over USB Serial/JTAG and never sent over Wi-Fi. */
            if (key_line_active) {
                if (byte == '\r') continue;
                if (byte == '\n') {
                    finish_key_line();
                    continue;
                }
                if (key_line_len + 1 >= sizeof(key_line)) {
                    key_line_active = false;
                    key_line_len = 0;
                    send_serial_text("KAGEKEY:ERROR\n");
                    continue;
                }
                key_line[key_line_len++] = static_cast<char>(byte);
                continue;
            }

            if (input_len == 0 && byte == 'K') {
                key_line_active = true;
                key_line_len = 0;
                key_line[key_line_len++] = 'K';
                continue;
            }

            if (input_len < sizeof(header)) {
                if (byte == header[input_len]) input[input_len++] = byte;
                else input_len = byte == header[0] ? 1 : 0;
                continue;
            }

            if (input_len >= sizeof(input)) {
                input_len = 0;
                continue;
            }

            input[input_len++] = byte;
            if (input_len < 9) continue;

            const size_t expected = 10 + input[8];
            if (input_len < expected) continue;

            if (input_len == expected) {
                uint8_t checksum = 0;
                for (size_t i = 0; i + 1 < input_len; ++i) checksum += input[i];

                if (checksum == input[input_len - 1] &&
                    input[6] == 1 &&
                    input[7] == TYPE_RPC) {
                    handle_rpc(input + 9, input[8]);
                } else if (checksum != input[input_len - 1]) {
                    send_error(0x01);
                }
            }

            input_len = 0;
        }
    }
}

static void wifi_event(void *, esp_event_base_t base, int32_t id, void *event_data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START && configured) {
        const int index = active_profile >= 0 ? active_profile : first_valid_profile();
        if (index >= 0) {
            configure_profile_in_ram(index);
            esp_wifi_connect();
        }
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const auto *event = static_cast<const wifi_event_sta_disconnected_t *>(event_data);
        const int32_t reason = event ? static_cast<int32_t>(event->reason) : -1;

        portENTER_CRITICAL(&info_lock);
        connected = false;
        last_disconnect_reason = reason;
        ip_address[0] = 0;
        gateway[0] = 0;
        subnet_mask[0] = 0;
        portEXIT_CRITICAL(&info_lock);

        ESP_LOGW("kage-wifi", "Disconnected from %s (reason=%ld)",
                 configured ? ssid : "<not configured>",
                 static_cast<long>(reason));
        event_log_add("Wi-Fi disconnected (reason %ld)", static_cast<long>(reason));

        if (configured) {
            ++reconnect_failures;

            if (reconnect_failures >= FAILURES_BEFORE_SWITCH) {
                const int alternate = other_valid_profile(active_profile);
                if (alternate >= 0) {
                    reconnect_failures = 0;
                    ESP_LOGI("kage-wifi", "Trying alternate Wi-Fi profile: %s",
                             profiles[alternate].ssid);
                    event_log_add("Trying Wi-Fi: %s", profiles[alternate].ssid);
                    configure_profile_in_ram(alternate);
                }
            }

            esp_wifi_connect();
        }
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const auto *event = static_cast<const ip_event_got_ip_t *>(event_data);

        char new_ip[16] = {};
        char new_gateway[16] = {};
        char new_mask[16] = {};
        if (event) {
            esp_ip4addr_ntoa(&event->ip_info.ip, new_ip, sizeof(new_ip));
            esp_ip4addr_ntoa(&event->ip_info.gw, new_gateway, sizeof(new_gateway));
            esp_ip4addr_ntoa(&event->ip_info.netmask, new_mask, sizeof(new_mask));
        }

        portENTER_CRITICAL(&info_lock);
        connected = true;
        last_disconnect_reason = 0;
        copy_text(ip_address, sizeof(ip_address), new_ip);
        copy_text(gateway, sizeof(gateway), new_gateway);
        copy_text(subnet_mask, sizeof(subnet_mask), new_mask);
        portEXIT_CRITICAL(&info_lock);

        reconnect_failures = 0;
        start_sntp_if_needed();
        start_status_server();

        ESP_LOGI("kage-wifi", "Connected to %s with IP %s", ssid, new_ip);
        event_log_add("Wi-Fi online: %s", new_ip[0] ? new_ip : "IP pending");

        send_error(0x00);
        send_state(STATE_PROVISIONED);
        if (provisioning_request) {
            send_empty_rpc_response(RPC_WIFI_SETTINGS);
            provisioning_request = false;
        }
    }
}
}  // namespace

void wifi_service_begin(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    /* Read the old single-profile station configuration before switching the
       Wi-Fi driver to RAM-backed configs. This imports the user's current Wi-Fi
       into Kage's own two-profile store exactly once. */
    wifi_config_t legacy = {};
    ESP_ERROR_CHECK(esp_wifi_get_config(WIFI_IF_STA, &legacy));

    load_profiles_from_nvs();
    load_api_key_from_nvs();
    import_legacy_profile_if_needed(legacy);

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, nullptr));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    active_profile = first_valid_profile();
    configured = active_profile >= 0;

    if (configured) {
        copy_text(ssid, sizeof(ssid), profiles[active_profile].ssid);
        ESP_LOGI("kage-wifi", "Loaded Wi-Fi profile %d: %s",
                 active_profile + 1, ssid);
        event_log_add("Stored Wi-Fi: %s", ssid);
        configure_profile_in_ram(active_profile);
    } else {
        ESP_LOGI("kage-wifi", "No stored Wi-Fi configuration");
        event_log_add("Wi-Fi not configured");
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    xTaskCreate(serial_task, "improv_serial", 4096, nullptr, 4, nullptr);
}

bool wifi_service_connected(void) {
    return connected;
}

bool wifi_service_is_configured(void) {
    return configured;
}

void wifi_service_forget(void) {
    esp_wifi_disconnect();

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
    }

    for (auto &profile : profiles) profile = {};
    api_key[0] = 0;
    active_profile = -1;
    reconnect_failures = 0;

    portENTER_CRITICAL(&info_lock);
    configured = false;
    connected = false;
    provisioning_request = false;
    last_disconnect_reason = 0;
    ssid[0] = 0;
    ip_address[0] = 0;
    gateway[0] = 0;
    subnet_mask[0] = 0;
    portEXIT_CRITICAL(&info_lock);

    event_log_add("Wi-Fi profiles cleared");
}

const char *wifi_service_name(void) {
    return configured ? ssid : "Non configuré";
}

int32_t wifi_service_last_disconnect_reason(void) {
    return last_disconnect_reason;
}

void wifi_service_get_info(WifiServiceInfo *info) {
    if (!info) return;
    std::memset(info, 0, sizeof(*info));

    portENTER_CRITICAL(&info_lock);
    info->configured = configured;
    info->connected = connected;
    info->last_disconnect_reason = last_disconnect_reason;
    copy_text(info->ssid, sizeof(info->ssid), ssid);
    copy_text(info->ip, sizeof(info->ip), ip_address);
    copy_text(info->gateway, sizeof(info->gateway), gateway);
    copy_text(info->mask, sizeof(info->mask), subnet_mask);
    portEXIT_CRITICAL(&info_lock);

    if (!info->connected) return;

    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        info->rssi = ap.rssi;
        info->channel = ap.primary;
        std::snprintf(info->bssid, sizeof(info->bssid),
                      "%02x:%02x:%02x:%02x:%02x:%02x",
                      ap.bssid[0], ap.bssid[1], ap.bssid[2],
                      ap.bssid[3], ap.bssid[4], ap.bssid[5]);
    }
}


bool wifi_service_has_api_key(void) {
    return api_key[0] != 0;
}

const char *wifi_service_api_key(void) {
    return api_key;
}

int wifi_service_active_profile_index(void) {
    return active_profile;
}
