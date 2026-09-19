#include "wifi_port.h"

#include <string.h>
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "esp_http_server.h"

#define WIFI_NAMESPACE "kage_wifi"
#define OTA_URL "https://oussnmr.github.io/kage-eyes/firmware/app.bin"
static const char *TAG = "kage_wifi";
static bool s_connected, s_portal, s_started;
static httpd_handle_t s_server;

static void decode(char *text) {
    char *read = text, *write = text;
    while (*read) {
        if (*read == '+' ) { *write++ = ' '; read++; continue; }
        if (*read == '%' && read[1] && read[2]) {
            unsigned value = 0;
            if (sscanf(read + 1, "%2x", &value) == 1) { *write++ = (char)value; read += 3; continue; }
        }
        *write++ = *read++;
    }
    *write = 0;
}

static bool form_value(char *form, const char *key, char *out, size_t out_size) {
    size_t key_len = strlen(key);
    for (char *part = form; part && *part;) {
        char *next = strchr(part, '&');
        if (next) *next = 0;
        if (strncmp(part, key, key_len) == 0 && part[key_len] == '=') {
            strncpy(out, part + key_len + 1, out_size - 1);
            out[out_size - 1] = 0;
            decode(out);
            return true;
        }
        part = next ? next + 1 : NULL;
    }
    return false;
}

static void restart_later(void *unused) { (void)unused; vTaskDelay(pdMS_TO_TICKS(1500)); esp_restart(); }

static esp_err_t page_get(httpd_req_t *req) {
    static const char page[] =
        "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<style>body{background:#000;color:#dff;font:17px system-ui;margin:2rem;max-width:38rem}"
        "input,button{box-sizing:border-box;width:100%;padding:14px;margin:7px 0;border-radius:9px;border:1px solid #277;background:#102;color:#dff}button{background:#3ff;color:#012;font-weight:bold}</style>"
        "<h1>Kage Eyes</h1><p>Connexion Wi-Fi locale</p><form method=post action=/save>"
        "<input name=ssid placeholder='Nom du Wi-Fi (SSID)' required><input name=password type=password placeholder='Mot de passe Wi-Fi'>"
        "<button>Enregistrer et connecter</button></form><p>Cette page est fournie par la carte. Les identifiants ne passent pas par GitHub.</p>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t save_post(httpd_req_t *req) {
    if (req->content_len <= 0 || req->content_len > 220) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form");
    char body[224] = {0};
    int received = httpd_req_recv(req, body, req->content_len);
    if (received <= 0) return ESP_FAIL;
    body[received] = 0;
    char ssid[33] = {0}, password[65] = {0};
    if (!form_value(body, "ssid", ssid, sizeof(ssid)) || !ssid[0]) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
    form_value(body, "password", password, sizeof(password));
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, "ssid", ssid));
    ESP_ERROR_CHECK(nvs_set_str(nvs, "pass", password));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);
    httpd_resp_sendstr(req, "Wi-Fi saved. Kage Eyes is restarting now.");
    xTaskCreate(restart_later, "restart_later", 2048, NULL, 1, NULL);
    return ESP_OK;
}

static void start_server(void) {
    if (s_server) return;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    if (httpd_start(&s_server, &config) != ESP_OK) return;
    httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = page_get};
    httpd_uri_t save = {.uri = "/save", .method = HTTP_POST, .handler = save_post};
    httpd_register_uri_handler(s_server, &root);
    httpd_register_uri_handler(s_server, &save);
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) esp_wifi_connect();
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) { s_connected = false; esp_wifi_connect(); }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) { s_connected = true; ESP_LOGI(TAG, "connected to Wi-Fi"); }
}

static void start_ap(void) {
    wifi_config_t ap = { .ap = { .ssid = "Kage-Eyes-Setup", .ssid_len = 16, .channel = 1,
        .max_connection = 2, .authmode = WIFI_AUTH_OPEN } };
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    s_portal = true;
    start_server();
    ESP_LOGI(TAG, "setup portal: Kage-Eyes-Setup at 192.168.4.1");
}

void wifi_port_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL));
    char ssid[33] = {0}, password[65] = {0};
    nvs_handle_t nvs;
    esp_err_t saved = nvs_open(WIFI_NAMESPACE, NVS_READONLY, &nvs);
    if (saved == ESP_OK) {
        size_t size = sizeof(ssid);
        saved = nvs_get_str(nvs, "ssid", ssid, &size);
        size = sizeof(password);
        nvs_get_str(nvs, "pass", password, &size);
        nvs_close(nvs);
    }
    wifi_config_t station = {0};
    memcpy(station.sta.ssid, ssid, strnlen(ssid, sizeof(station.sta.ssid) - 1));
    memcpy(station.sta.password, password, strnlen(password, sizeof(station.sta.password) - 1));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    if (saved == ESP_OK) ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &station));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_started = true;
    if (saved != ESP_OK) start_ap();
}

void wifi_port_begin_setup(void) { if (s_started) start_ap(); }
bool wifi_port_connected(void) { return s_connected; }
bool wifi_port_setup_active(void) { return s_portal; }

static void ota_task(void *unused) {
    (void)unused;
    if (!s_connected) { vTaskDelete(NULL); return; }
    esp_http_client_config_t http = {.url = OTA_URL, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 15000};
    esp_https_ota_config_t ota = {.http_config = &http};
    if (esp_https_ota(&ota) == ESP_OK) esp_restart();
    ESP_LOGW(TAG, "OTA check/download failed; current firmware retained");
    vTaskDelete(NULL);
}
void wifi_port_start_update(void) { if (s_connected) xTaskCreate(ota_task, "kage_ota", 8192, NULL, 4, NULL); }
