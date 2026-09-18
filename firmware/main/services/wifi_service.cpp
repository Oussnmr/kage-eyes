/* Improv Wi-Fi Serial service. It deliberately has no SoftAP and no local
 * web server: credentials are sent through USB Serial/JTAG straight to the
 * ESP32's normal Wi-Fi station configuration. */
#include "wifi_service.h"

#include <cstring>

#include "driver/usb_serial_jtag.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

static bool connected;
static bool configured;
static bool provisioning_request;
static char ssid[33] = {};
static uint8_t input[320];
static size_t input_len;

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

    wifi_config_t config = {};
    memcpy(config.sta.ssid, data + 3, ssid_len);
    config.sta.ssid[ssid_len] = 0;
    memcpy(config.sta.password, data + 4 + ssid_len, pass_len);
    config.sta.password[pass_len] = 0;

    if (esp_wifi_set_config(WIFI_IF_STA, &config) != ESP_OK) {
        send_error(0xFF);
        return;
    }

    memcpy(ssid, config.sta.ssid, sizeof(ssid));
    configured = true;
    connected = false;
    provisioning_request = true;

    send_state(STATE_PROVISIONING);
    esp_wifi_disconnect();
    esp_wifi_connect();
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

static void wifi_event(void *, esp_event_base_t base, int32_t id, void *) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START && configured) {
        esp_wifi_connect();
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        connected = false;
        if (configured) esp_wifi_connect();
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        connected = true;
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
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, nullptr));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_config_t stored = {};
    ESP_ERROR_CHECK(esp_wifi_get_config(WIFI_IF_STA, &stored));
    configured = stored.sta.ssid[0] != 0;
    if (configured) memcpy(ssid, stored.sta.ssid, sizeof(ssid));

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
    esp_wifi_restore();
    configured = false;
    connected = false;
    provisioning_request = false;
    ssid[0] = 0;
}

const char *wifi_service_name(void) {
    return configured ? ssid : "Non configuré";
}
