#include "wifi_prov.h"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "blufi_example.h"
#include "cJSON.h"
#include "config.h"
#include "esp_blufi.h"
#include "esp_blufi_api.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "geiger.h"
#include "history_store.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "mqtt.h"
#include "nvs_flash.h"
#include "settings.h"
#include "soc/soc_caps.h"
#include "esp_sntp.h"

static const char *TAG = "wifi";

#define WIFI_SCAN_MAX       16
#define INVALID_RSSI        (-127)
#define INVALID_REASON      255

static esp_netif_t        *s_sta_netif;
static esp_netif_t        *s_ap_netif;
static httpd_handle_t      s_httpd;
static int                 s_dns_sock = -1;
static wifi_prov_method_t  s_running;
static char                s_ap_ssid[32];
static esp_timer_handle_t  s_portal_timer;
static bool                s_sntp_started;

static void portal_stop(void);

static void time_sync_cb(struct timeval *tv)
{
    time_t now = time(NULL);
    struct tm t = {0};
    localtime_r(&now, &t);
    ESP_LOGI(TAG, "clock synced: %04d-%02d-%02d %02d:%02d:%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);
}

static void time_sync_start(void)
{
    if (s_sntp_started) {
        return;
    }
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_set_time_sync_notification_cb(time_sync_cb);
    sntp_init();
    s_sntp_started = true;
    ESP_LOGI(TAG, "SNTP started");
}

/* Station state, also reported back over BluFi. */
static bool     s_sta_connected;
static bool     s_sta_got_ip;
static bool     s_sta_connecting;
static bool     s_ble_connected;
static uint8_t  s_retries;
static uint8_t  s_sta_bssid[6];
static uint8_t  s_sta_ssid[32];
static int      s_sta_ssid_len;
static esp_blufi_extra_info_t s_conn_info;

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");

/* =========================================================================
 * Station handling
 * ====================================================================== */

static int softap_connection_number(void)
{
    wifi_sta_list_t list;
    return esp_wifi_ap_get_sta_list(&list) == ESP_OK ? list.num : 0;
}

static void record_conn_info(int rssi, uint8_t reason)
{
    memset(&s_conn_info, 0, sizeof(s_conn_info));
    if (s_sta_connecting) {
        s_conn_info.sta_max_conn_retry_set = true;
        s_conn_info.sta_max_conn_retry     = WIFI_STA_MAX_RETRY;
    } else {
        s_conn_info.sta_conn_rssi_set       = true;
        s_conn_info.sta_conn_rssi           = rssi;
        s_conn_info.sta_conn_end_reason_set = true;
        s_conn_info.sta_conn_end_reason     = reason;
    }
}

static void sta_connect(void)
{
    s_retries = 0;
    s_sta_connecting = (esp_wifi_connect() == ESP_OK);
    record_conn_info(INVALID_RSSI, INVALID_REASON);
}

static bool sta_reconnect(void)
{
    if (!s_sta_connecting || s_retries++ >= WIFI_STA_MAX_RETRY) {
        return false;
    }
    s_sta_connecting = (esp_wifi_connect() == ESP_OK);
    record_conn_info(INVALID_RSSI, INVALID_REASON);
    return true;
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    wifi_mode_t mode;

    switch (id) {
    case WIFI_EVENT_STA_START:
        if (wifi_has_credentials()) {
            sta_connect();
        }
        break;

    case WIFI_EVENT_STA_CONNECTED: {
        wifi_event_sta_connected_t *e = data;
        s_sta_connected  = true;
        s_sta_connecting = false;
        memcpy(s_sta_bssid, e->bssid, 6);
        memcpy(s_sta_ssid, e->ssid, e->ssid_len);
        s_sta_ssid_len = e->ssid_len;
        ESP_LOGI(TAG, "connected to %.*s", e->ssid_len, e->ssid);
        break;
    }

    case WIFI_EVENT_STA_DISCONNECTED: {
        wifi_event_sta_disconnected_t *e = data;
        bool retrying = sta_reconnect();
        s_sta_connected = false;
        s_sta_got_ip    = false;

        if (!retrying) {
            s_sta_connecting = false;
            record_conn_info(e->rssi, e->reason);
            ESP_LOGW(TAG, "disconnected, reason %d", e->reason);
            if (s_ble_connected) {
                esp_wifi_get_mode(&mode);
                esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_FAIL,
                                                softap_connection_number(), &s_conn_info);
            }
        }
        break;
    }

    default:
        break;
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id != IP_EVENT_STA_GOT_IP) {
        return;
    }
    s_sta_got_ip = true;
    ESP_LOGI(TAG, "got ip " IPSTR ", web ui on http://" WIFI_MDNS_HOSTNAME ".local",
             IP2STR(&((ip_event_got_ip_t *)data)->ip_info.ip));
    time_sync_start();

    /* Give the portal client a moment to follow the redirect, then drop the SoftAP. */
    if (WIFI_PORTAL_LINGER_MS > 0 && (s_running & WIFI_PROV_SOFTAP) && s_portal_timer &&
        !esp_timer_is_active(s_portal_timer)) {
        esp_timer_start_once(s_portal_timer, (uint64_t)WIFI_PORTAL_LINGER_MS * 1000);
    }

    if (s_ble_connected) {
        wifi_mode_t mode;
        esp_blufi_extra_info_t info = {0};
        esp_wifi_get_mode(&mode);
        memcpy(info.sta_bssid, s_sta_bssid, 6);
        info.sta_bssid_set = true;
        info.sta_ssid      = s_sta_ssid;
        info.sta_ssid_len  = s_sta_ssid_len;
        esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_SUCCESS,
                                        softap_connection_number(), &info);
    }
}

/* =========================================================================
 * BluFi (BLE provisioning)
 * ====================================================================== */

static void blufi_report_status(void)
{
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);

    if (s_sta_connected) {
        esp_blufi_extra_info_t info = {0};
        memcpy(info.sta_bssid, s_sta_bssid, 6);
        info.sta_bssid_set = true;
        info.sta_ssid      = s_sta_ssid;
        info.sta_ssid_len  = s_sta_ssid_len;
        esp_blufi_send_wifi_conn_report(mode,
                                        s_sta_got_ip ? ESP_BLUFI_STA_CONN_SUCCESS : ESP_BLUFI_STA_NO_IP,
                                        softap_connection_number(), &info);
    } else if (s_sta_connecting) {
        esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONNECTING,
                                        softap_connection_number(), &s_conn_info);
    } else {
        esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_FAIL,
                                        softap_connection_number(), &s_conn_info);
    }
}

static void blufi_send_ap_list(void)
{
    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    if (count == 0) {
        return;
    }

    wifi_ap_record_t *records = calloc(count, sizeof(wifi_ap_record_t));
    if (!records) {
        esp_wifi_clear_ap_list();
        return;
    }
    if (esp_wifi_scan_get_ap_records(&count, records) != ESP_OK) {
        free(records);
        return;
    }

    esp_blufi_ap_record_t *list = calloc(count, sizeof(esp_blufi_ap_record_t));
    if (list) {
        for (int i = 0; i < count; i++) {
            list[i].rssi = records[i].rssi;
            memcpy(list[i].ssid, records[i].ssid, sizeof(list[i].ssid));
        }
        esp_blufi_send_wifi_list(count, list);
        free(list);
    }
    free(records);
    esp_wifi_scan_stop();
}

static void blufi_event_callback(esp_blufi_cb_event_t event, esp_blufi_cb_param_t *param)
{
    static wifi_config_t sta_cfg;
    static wifi_config_t ap_cfg;

    switch (event) {
    case ESP_BLUFI_EVENT_INIT_FINISH:
        ESP_LOGI(TAG, "blufi ready");
#if SOC_MPI_SUPPORTED
        esp_blufi_adv_start();
#endif
        break;

    case ESP_BLUFI_EVENT_BLE_CONNECT:
        s_ble_connected = true;
        esp_blufi_adv_stop();
        blufi_security_init();
        break;

    case ESP_BLUFI_EVENT_BLE_DISCONNECT:
        s_ble_connected = false;
        blufi_security_deinit();
#if SOC_MPI_SUPPORTED
        esp_blufi_adv_start();
#else
        blufi_dh_pregen_start_with_cb(esp_blufi_adv_start);
#endif
        break;

    case ESP_BLUFI_EVENT_SET_WIFI_OPMODE:
        esp_wifi_set_mode(param->wifi_mode.op_mode);
        break;

    case ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP:
        esp_wifi_disconnect();
        esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
        sta_connect();
        break;

    case ESP_BLUFI_EVENT_REQ_DISCONNECT_FROM_AP:
        esp_wifi_disconnect();
        break;

    case ESP_BLUFI_EVENT_RECV_STA_SSID:
        if (param->sta_ssid.ssid_len >= sizeof(sta_cfg.sta.ssid)) {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            break;
        }
        memset(sta_cfg.sta.ssid, 0, sizeof(sta_cfg.sta.ssid));
        memcpy(sta_cfg.sta.ssid, param->sta_ssid.ssid, param->sta_ssid.ssid_len);
        break;

    case ESP_BLUFI_EVENT_RECV_STA_PASSWD:
        if (param->sta_passwd.passwd_len >= sizeof(sta_cfg.sta.password)) {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            break;
        }
        memset(sta_cfg.sta.password, 0, sizeof(sta_cfg.sta.password));
        memcpy(sta_cfg.sta.password, param->sta_passwd.passwd, param->sta_passwd.passwd_len);
        break;

    case ESP_BLUFI_EVENT_RECV_STA_BSSID:
        memcpy(sta_cfg.sta.bssid, param->sta_bssid.bssid, sizeof(sta_cfg.sta.bssid));
        sta_cfg.sta.bssid_set = 1;
        break;

    case ESP_BLUFI_EVENT_RECV_SOFTAP_SSID:
        if (param->softap_ssid.ssid_len >= sizeof(ap_cfg.ap.ssid)) {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            break;
        }
        memset(ap_cfg.ap.ssid, 0, sizeof(ap_cfg.ap.ssid));
        memcpy(ap_cfg.ap.ssid, param->softap_ssid.ssid, param->softap_ssid.ssid_len);
        ap_cfg.ap.ssid_len = param->softap_ssid.ssid_len;
        esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
        break;

    case ESP_BLUFI_EVENT_RECV_SOFTAP_PASSWD:
        if (param->softap_passwd.passwd_len >= sizeof(ap_cfg.ap.password)) {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            break;
        }
        memset(ap_cfg.ap.password, 0, sizeof(ap_cfg.ap.password));
        memcpy(ap_cfg.ap.password, param->softap_passwd.passwd, param->softap_passwd.passwd_len);
        esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
        break;

    case ESP_BLUFI_EVENT_GET_WIFI_LIST: {
        wifi_scan_config_t scan = { .show_hidden = true };
        if (esp_wifi_scan_start(&scan, true) != ESP_OK) {
            esp_blufi_send_error_info(ESP_BLUFI_WIFI_SCAN_FAIL);
        } else {
            blufi_send_ap_list();
        }
        break;
    }

    case ESP_BLUFI_EVENT_GET_WIFI_STATUS:
        blufi_report_status();
        break;

    case ESP_BLUFI_EVENT_REPORT_ERROR:
        esp_blufi_send_error_info(param->report_error.state);
        break;

    case ESP_BLUFI_EVENT_RECV_SLAVE_DISCONNECT_BLE:
        esp_blufi_disconnect();
        break;

    default:
        break;
    }
}

static esp_blufi_callbacks_t s_blufi_callbacks = {
    .event_cb               = blufi_event_callback,
    .negotiate_data_handler = blufi_dh_negotiate_data_handler,
    .encrypt_func           = blufi_aes_encrypt,
    .decrypt_func           = blufi_aes_decrypt,
    .checksum_func          = blufi_crc_checksum,
};

static esp_err_t blufi_start(void)
{
#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
    ESP_RETURN_ON_ERROR(esp_blufi_controller_init(), TAG, "bt controller");
#endif
    ESP_RETURN_ON_ERROR(esp_blufi_host_and_cb_init(&s_blufi_callbacks), TAG, "blufi host");
#if !SOC_MPI_SUPPORTED
    blufi_dh_pregen_start();
    blufi_dh_pregen_wait();
    esp_blufi_adv_start();
#endif
    return ESP_OK;
}

static void blufi_stop(void)
{
    esp_blufi_host_deinit();
#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
    esp_blufi_controller_deinit();
#endif
    s_ble_connected = false;
}

/* =========================================================================
 * Web UI: landing page + JSON API, served on both the SoftAP and the station
 * ====================================================================== */

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, body);
    cJSON_free(body);
    return err;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static esp_err_t live_get_handler(httpd_req_t *req)
{
    int hv_mv = 0;
    int vbat_mv = 0;
    board_adc_get_mv(BOARD_ADC_TUBE, &hv_mv);
    board_adc_get_mv(BOARD_ADC_VLATCH, &vbat_mv);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "cpm", geiger_cpm());
    cJSON_AddNumberToObject(root, "usvh", geiger_usvh());
    cJSON_AddNumberToObject(root, "total", geiger_total());
    cJSON_AddNumberToObject(root, "hv_mv", hv_mv);
    cJSON_AddNumberToObject(root, "vbat_mv", vbat_mv);
    cJSON_AddBoolToObject(root, "hv_ok", board_input_level(BOARD_IN_VTUBE_OK));
    cJSON_AddNumberToObject(root, "uptime", esp_timer_get_time() / 1000000);
    return send_json(req, root);
}

static esp_err_t history_get_handler(httpd_req_t *req)
{
    uint32_t window_s = settings_get()->history_short_window_s;
    char query[80];
    if (httpd_req_get_url_query_len(req) > 0 &&
        httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[16];
        if (httpd_query_key_value(query, "window_s", val, sizeof(val)) == ESP_OK) {
            uint32_t parsed = (uint32_t)strtoul(val, NULL, 10);
            if (parsed >= HISTORY_SHORT_MIN_S && parsed <= HISTORY_SHORT_MAX_S) {
                window_s = parsed;
            }
        }
    }

    uint32_t now_s = (uint32_t)time(NULL);
    uint32_t since = (now_s > window_s) ? (now_s - window_s) : 0;

    history_point_t *points = calloc(720, sizeof(history_point_t));
    if (!points) {
        return httpd_resp_send_500(req);
    }
    size_t n = 0;
    uint32_t oldest = 0;
    uint32_t newest = 0;
    esp_err_t hist_err = history_store_query(since, points, 720, &n, &oldest, &newest);
    if (hist_err != ESP_OK) {
        free(points);
        ESP_LOGE(TAG, "history read failed: %s", esp_err_to_name(hist_err));
        return httpd_resp_send_500(req);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "window_s", window_s);
    cJSON_AddNumberToObject(root, "now", now_s);
    cJSON_AddNumberToObject(root, "oldest", oldest);
    cJSON_AddNumberToObject(root, "newest", newest);
    cJSON_AddNumberToObject(root, "cpm_per_usvh", settings_get()->tube_cpm_per_usvh);

    cJSON *arr = cJSON_AddArrayToObject(root, "cpm");
    cJSON *ts_arr = cJSON_AddArrayToObject(root, "ts");
    for (size_t i = 0; i < n; i++) {
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(points[i].cpm));
        cJSON_AddItemToArray(ts_arr, cJSON_CreateNumber(points[i].ts));
    }
    esp_err_t resp = send_json(req, root);
    free(points);
    return resp;
}

static esp_err_t history_csv_get_handler(httpd_req_t *req)
{
    FILE *f = fopen(HISTORY_STORE_FILE_PATH, "r");
    if (!f) {
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "");
    }

    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    char chunk[512];
    while (fgets(chunk, sizeof(chunk), f)) {
        if (httpd_resp_send_chunk(req, chunk, HTTPD_RESP_USE_STRLEN) != ESP_OK) {
            fclose(f);
            return ESP_FAIL;
        }
    }
    fclose(f);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t scan_get_handler(httpd_req_t *req)
{
    wifi_scan_config_t scan = { .show_hidden = false };
    if (esp_wifi_scan_start(&scan, true) != ESP_OK) {
        return httpd_resp_send_500(req);
    }

    uint16_t count = WIFI_SCAN_MAX;
    wifi_ap_record_t records[WIFI_SCAN_MAX];
    if (esp_wifi_scan_get_ap_records(&count, records) != ESP_OK) {
        return httpd_resp_send_500(req);
    }

    cJSON *root = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        if (records[i].ssid[0] == '\0') {
            continue;
        }
        cJSON *ap = cJSON_CreateObject();
        cJSON_AddStringToObject(ap, "ssid", (const char *)records[i].ssid);
        cJSON_AddNumberToObject(ap, "rssi", records[i].rssi);
        cJSON_AddBoolToObject(ap, "lock", records[i].authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(root, ap);
    }
    return send_json(req, root);
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    char ip[16];
    wifi_get_ip_str(ip, sizeof(ip));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "connected", s_sta_got_ip);
    cJSON_AddBoolToObject(root, "connecting", s_sta_connecting);
    cJSON_AddBoolToObject(root, "portal", (s_running & WIFI_PROV_SOFTAP) != 0);
    cJSON_AddStringToObject(root, "ip", ip);
    cJSON_AddStringToObject(root, "hostname", WIFI_MDNS_HOSTNAME);
    cJSON_AddStringToObject(root, "ssid", (const char *)s_sta_ssid);
    return send_json(req, root);
}

static esp_err_t connect_post_handler(httpd_req_t *req)
{
    char body[256];
    if (req->content_len >= sizeof(body)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "payload too large");
    }
    int len = httpd_req_recv(req, body, req->content_len);
    if (len <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no payload");
    }
    body[len] = '\0';

    cJSON *root = cJSON_Parse(body);
    const cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
    const cJSON *pass = cJSON_GetObjectItem(root, "password");
    if (!cJSON_IsString(ssid) || strlen(ssid->valuestring) == 0) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid missing");
    }

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, ssid->valuestring, sizeof(cfg.sta.ssid));
    if (cJSON_IsString(pass)) {
        strlcpy((char *)cfg.sta.password, pass->valuestring, sizeof(cfg.sta.password));
    }
    cJSON_Delete(root);

    ESP_LOGI(TAG, "portal requested connection to %s", cfg.sta.ssid);
    esp_wifi_disconnect();
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        return httpd_resp_send_500(req);
    }
    sta_connect();

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* -------------------------------------------------------------------------
 * Persisted settings
 * ---------------------------------------------------------------------- */

static void json_get_str(const cJSON *root, const char *key, char *dst, size_t len)
{
    const cJSON *item = cJSON_GetObjectItem(root, key);
    if (cJSON_IsString(item)) {
        strlcpy(dst, item->valuestring, len);
    }
}

static void json_get_bool(const cJSON *root, const char *key, bool *dst)
{
    const cJSON *item = cJSON_GetObjectItem(root, key);
    if (cJSON_IsBool(item)) {
        *dst = cJSON_IsTrue(item);
    }
}

static void json_get_u16(const cJSON *root, const char *key, uint16_t *dst)
{
    const cJSON *item = cJSON_GetObjectItem(root, key);
    if (cJSON_IsNumber(item) && item->valuedouble > 0 && item->valuedouble < UINT16_MAX) {
        *dst = (uint16_t)item->valuedouble;
    }
}

static void json_get_u32(const cJSON *root, const char *key, uint32_t *dst)
{
    const cJSON *item = cJSON_GetObjectItem(root, key);
    if (cJSON_IsNumber(item) && item->valuedouble >= 0 && item->valuedouble <= UINT32_MAX) {
        *dst = (uint32_t)item->valuedouble;
    }
}

static void json_get_u8(const cJSON *root, const char *key, uint8_t *dst)
{
    const cJSON *item = cJSON_GetObjectItem(root, key);
    if (cJSON_IsNumber(item) && item->valuedouble >= 0 && item->valuedouble <= UINT8_MAX) {
        *dst = (uint8_t)item->valuedouble;
    }
}

static void json_get_float(const cJSON *root, const char *key, float *dst)
{
    const cJSON *item = cJSON_GetObjectItem(root, key);
    if (cJSON_IsNumber(item)) {
        *dst = (float)item->valuedouble;
    }
}

static esp_err_t settings_get_handler(httpd_req_t *req)
{
    const settings_t *cfg = settings_get();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_name", cfg->device_name);
    cJSON_AddStringToObject(root, "device_id", mqtt_device_id());
    cJSON_AddBoolToObject(root, "mqtt_enabled", cfg->mqtt_enabled);
    cJSON_AddStringToObject(root, "mqtt_uri", cfg->mqtt_uri);
    cJSON_AddStringToObject(root, "mqtt_user", cfg->mqtt_user);
    cJSON_AddBoolToObject(root, "mqtt_pass_set", cfg->mqtt_pass[0] != '\0');
    cJSON_AddStringToObject(root, "mqtt_topic", cfg->mqtt_topic);
    cJSON_AddBoolToObject(root, "mqtt_discovery", cfg->mqtt_discovery);
    cJSON_AddStringToObject(root, "mqtt_ha_prefix", cfg->mqtt_ha_prefix);
    cJSON_AddNumberToObject(root, "mqtt_interval_s", cfg->mqtt_interval_s);
    cJSON_AddStringToObject(root, "mqtt_state", mqtt_state_str());
    cJSON_AddNumberToObject(root, "tube_window_s", cfg->tube_window_s);
    cJSON_AddNumberToObject(root, "tube_cpm_per_usvh", cfg->tube_cpm_per_usvh);
    cJSON_AddNumberToObject(root, "tube_target_v", cfg->tube_target_v);
    cJSON_AddNumberToObject(root, "spk_volume", cfg->spk_volume);
    cJSON_AddNumberToObject(root, "spk_sound", cfg->spk_sound);
    cJSON_AddBoolToObject(root, "batt_charge_en", cfg->batt_charge_en);
    cJSON_AddBoolToObject(root, "led_enabled", cfg->led_enabled);
    cJSON_AddNumberToObject(root, "lcd_brightness", cfg->lcd_brightness);
    cJSON_AddBoolToObject(root, "lcd_auto_dim", cfg->lcd_auto_dim);
    cJSON_AddNumberToObject(root, "history_short_window_s", cfg->history_short_window_s);
    return send_json(req, root);
}

static esp_err_t settings_post_handler(httpd_req_t *req)
{
    char body[768];
    if (req->content_len >= sizeof(body)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "payload too large");
    }
    int len = httpd_req_recv(req, body, req->content_len);
    if (len <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no payload");
    }
    body[len] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    }

    /* Fields left out of the request keep their current value. */
    settings_t cfg = *settings_get();
    json_get_str(root, "device_name", cfg.device_name, sizeof(cfg.device_name));
    json_get_bool(root, "mqtt_enabled", &cfg.mqtt_enabled);
    json_get_str(root, "mqtt_uri", cfg.mqtt_uri, sizeof(cfg.mqtt_uri));
    json_get_str(root, "mqtt_user", cfg.mqtt_user, sizeof(cfg.mqtt_user));
    json_get_str(root, "mqtt_pass", cfg.mqtt_pass, sizeof(cfg.mqtt_pass));
    json_get_str(root, "mqtt_topic", cfg.mqtt_topic, sizeof(cfg.mqtt_topic));
    json_get_bool(root, "mqtt_discovery", &cfg.mqtt_discovery);
    json_get_str(root, "mqtt_ha_prefix", cfg.mqtt_ha_prefix, sizeof(cfg.mqtt_ha_prefix));
    json_get_u16(root, "mqtt_interval_s", &cfg.mqtt_interval_s);
    json_get_u16(root, "tube_window_s", &cfg.tube_window_s);
    json_get_float(root, "tube_cpm_per_usvh", &cfg.tube_cpm_per_usvh);
    json_get_u16(root, "tube_target_v", &cfg.tube_target_v);
    json_get_u8(root, "spk_volume", &cfg.spk_volume);
    json_get_u8(root, "spk_sound", &cfg.spk_sound);
    json_get_bool(root, "batt_charge_en", &cfg.batt_charge_en);
    json_get_bool(root, "led_enabled", &cfg.led_enabled);
    json_get_u8(root, "lcd_brightness", &cfg.lcd_brightness);
    json_get_bool(root, "lcd_auto_dim", &cfg.lcd_auto_dim);
    json_get_u32(root, "history_short_window_s", &cfg.history_short_window_s);
    cJSON_Delete(root);

    if (settings_save(&cfg) != ESP_OK) {
        return httpd_resp_send_500(req);
    }
    board_apply_settings();
    mqtt_apply();

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "mqtt_state", mqtt_state_str());
    return send_json(req, resp);
}

static void restart_cb(void *arg)
{
    esp_restart();
}

static esp_err_t restart_post_handler(httpd_req_t *req)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    ESP_RETURN_ON_ERROR(send_json(req, resp), TAG, "restart response");

    esp_timer_handle_t timer;
    esp_timer_create_args_t args = {
        .callback = restart_cb,
        .name = "restart",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&args, &timer), TAG, "restart timer");
    return esp_timer_start_once(timer, 250000);
}

static esp_err_t factory_reset_post_handler(httpd_req_t *req)
{
    if (history_store_format() != ESP_OK) {
        return httpd_resp_send_500(req);
    }
    if (nvs_flash_erase() != ESP_OK) {
        return httpd_resp_send_500(req);
    }
    return restart_post_handler(req);
}

/* While the portal runs, anything unknown is redirected so phones pop up the sign-in page. */
static esp_err_t redirect_handler(httpd_req_t *req)
{
    if (!(s_running & WIFI_PROV_SOFTAP)) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" WIFI_AP_IP "/");
    return httpd_resp_send(req, NULL, 0);
}

/* =========================================================================
 * Captive portal DNS: answers every A query with the SoftAP address
 * ====================================================================== */

#define DNS_PORT        53
#define DNS_MAX_LEN     256
#define DNS_TYPE_A      1
#define DNS_CLASS_IN    1

typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qd_count;
    uint16_t an_count;
    uint16_t ns_count;
    uint16_t ar_count;
} dns_header_t;

typedef struct __attribute__((packed)) {
    uint16_t type;
    uint16_t cls;
} dns_question_t;

typedef struct __attribute__((packed)) {
    uint16_t name_ptr;
    uint16_t type;
    uint16_t cls;
    uint32_t ttl;
    uint16_t addr_len;
    uint32_t addr;
} dns_answer_t;

static int dns_build_reply(uint8_t *buf, int len, int cap)
{
    dns_header_t hdr;
    if (len < (int)sizeof(hdr)) {
        return 0;
    }
    memcpy(&hdr, buf, sizeof(hdr));
    if ((ntohs(hdr.flags) & 0x8000) || ntohs(hdr.qd_count) != 1) {
        return 0;
    }

    int p = sizeof(hdr);
    while (p < len && buf[p]) {
        if (buf[p] & 0xC0) {
            return 0;  /* compression pointers are not expected in a query */
        }
        p += buf[p] + 1;
    }
    p++;
    if (p + (int)sizeof(dns_question_t) > len) {
        return 0;
    }

    dns_question_t q;
    memcpy(&q, &buf[p], sizeof(q));
    p += sizeof(q);
    if (ntohs(q.type) != DNS_TYPE_A || ntohs(q.cls) != DNS_CLASS_IN) {
        return 0;
    }
    if (p + (int)sizeof(dns_answer_t) > cap) {
        return 0;
    }

    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_ap_netif, &ip) != ESP_OK) {
        return 0;
    }

    dns_answer_t answer = {
        .name_ptr = htons(0xC000 | sizeof(dns_header_t)),
        .type     = htons(DNS_TYPE_A),
        .cls      = htons(DNS_CLASS_IN),
        .ttl      = htonl(60),
        .addr_len = htons(sizeof(answer.addr)),
        .addr     = ip.ip.addr,
    };
    memcpy(&buf[p], &answer, sizeof(answer));
    p += sizeof(answer);

    hdr.flags    = htons(0x8180);  /* response, recursion available */
    hdr.an_count = htons(1);
    hdr.ns_count = 0;
    hdr.ar_count = 0;
    memcpy(buf, &hdr, sizeof(hdr));
    return p;
}

static void dns_task(void *arg)
{
    uint8_t buf[DNS_MAX_LEN];

    for (;;) {
        struct sockaddr_storage src;
        socklen_t src_len = sizeof(src);

        int len = recvfrom(s_dns_sock, buf, sizeof(buf), 0, (struct sockaddr *)&src, &src_len);
        if (len < 0) {
            break;  /* socket closed by portal_stop() */
        }
        int reply = dns_build_reply(buf, len, sizeof(buf));
        if (reply > 0) {
            sendto(s_dns_sock, buf, reply, 0, (struct sockaddr *)&src, src_len);
        }
    }
    vTaskDelete(NULL);
}

static esp_err_t dns_start(void)
{
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port        = htons(DNS_PORT),
    };

    s_dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ESP_RETURN_ON_FALSE(s_dns_sock >= 0, ESP_FAIL, TAG, "dns socket");

    if (bind(s_dns_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(s_dns_sock);
        s_dns_sock = -1;
        ESP_RETURN_ON_FALSE(false, ESP_FAIL, TAG, "dns bind");
    }
    if (xTaskCreate(dns_task, "dns", 3072, NULL, 4, NULL) != pdPASS) {
        close(s_dns_sock);
        s_dns_sock = -1;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void dns_stop(void)
{
    if (s_dns_sock >= 0) {
        shutdown(s_dns_sock, SHUT_RDWR);
        close(s_dns_sock);
        s_dns_sock = -1;
    }
}

static esp_err_t http_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = WIFI_HTTP_PORT;
    cfg.max_uri_handlers = 12;
    cfg.stack_size       = 8192;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn     = httpd_uri_match_wildcard;
    ESP_RETURN_ON_ERROR(httpd_start(&s_httpd, &cfg), TAG, "httpd");

    /* Order matters: the wildcard entry must be registered last. */
    static const httpd_uri_t routes[] = {
        { .uri = "/",             .method = HTTP_GET,  .handler = root_get_handler },
        { .uri = "/api/live",     .method = HTTP_GET,  .handler = live_get_handler },
        { .uri = "/api/history",  .method = HTTP_GET,  .handler = history_get_handler },
        { .uri = "/api/history_csv", .method = HTTP_GET, .handler = history_csv_get_handler },
        { .uri = "/api/scan",     .method = HTTP_GET,  .handler = scan_get_handler },
        { .uri = "/api/status",   .method = HTTP_GET,  .handler = status_get_handler },
        { .uri = "/api/settings", .method = HTTP_GET,  .handler = settings_get_handler },
        { .uri = "/api/settings", .method = HTTP_POST, .handler = settings_post_handler },
        { .uri = "/api/connect",  .method = HTTP_POST, .handler = connect_post_handler },
        { .uri = "/api/restart",  .method = HTTP_POST, .handler = restart_post_handler },
        { .uri = "/api/factory_reset", .method = HTTP_POST, .handler = factory_reset_post_handler },
        { .uri = "/*",            .method = HTTP_GET,  .handler = redirect_handler },
    };
    for (int i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &routes[i]), TAG, "route %s", routes[i].uri);
    }
    return ESP_OK;
}

static esp_err_t mdns_start(void)
{
    ESP_RETURN_ON_ERROR(mdns_init(), TAG, "mdns init");
    ESP_RETURN_ON_ERROR(mdns_hostname_set(WIFI_MDNS_HOSTNAME), TAG, "mdns hostname");
    ESP_RETURN_ON_ERROR(mdns_instance_name_set(WIFI_MDNS_INSTANCE), TAG, "mdns instance");

    mdns_txt_item_t txt[] = { { "path", "/" } };
    ESP_RETURN_ON_ERROR(mdns_service_add(NULL, "_http", "_tcp", WIFI_HTTP_PORT, txt, 1), TAG, "mdns service");
    return ESP_OK;
}

static void portal_linger_cb(void *arg)
{
    if (s_running & WIFI_PROV_SOFTAP) {
        ESP_LOGI(TAG, "station is up, closing the captive portal");
        portal_stop();
        s_running &= ~WIFI_PROV_SOFTAP;
    }
}

static esp_err_t portal_start(void)
{
    wifi_config_t ap = {
        .ap = {
            .channel        = WIFI_AP_CHANNEL,
            .max_connection = WIFI_AP_MAX_CONN,
            .authmode       = sizeof(WIFI_AP_PASSWORD) > 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN,
            .pmf_cfg        = { .required = false },
        },
    };
    strlcpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(s_ap_ssid);
    strlcpy((char *)ap.ap.password, WIFI_AP_PASSWORD, sizeof(ap.ap.password));

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "apsta mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "ap config");

    /* DHCP option 114 makes modern clients open the portal on their own. */
    char uri[] = "http://" WIFI_AP_IP;
    esp_netif_dhcps_stop(s_ap_netif);
    esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, uri, sizeof(uri));
    esp_netif_dhcps_start(s_ap_netif);

    ESP_RETURN_ON_ERROR(dns_start(), TAG, "dns");

    ESP_LOGI(TAG, "portal up: connect to \"%s\" then open http://%s", s_ap_ssid, WIFI_AP_IP);
    return ESP_OK;
}

static void portal_stop(void)
{
    dns_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
}

/* =========================================================================
 * Public API
 * ====================================================================== */

esp_err_t wifi_prov_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs init");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif  = esp_netif_create_default_wifi_ap();
    ESP_RETURN_ON_FALSE(s_sta_netif && s_ap_netif, ESP_FAIL, TAG, "netif create");
    esp_netif_set_hostname(s_sta_netif, WIFI_MDNS_HOSTNAME);

    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL),
                        TAG, "wifi events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_handler, NULL),
                        TAG, "ip events");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_FLASH), TAG, "wifi storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "sta mode");

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s-%02X%02X%02X", WIFI_AP_SSID_PREFIX, mac[3], mac[4], mac[5]);

    record_conn_info(INVALID_RSSI, INVALID_REASON);
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");

    ESP_RETURN_ON_ERROR(mdns_start(), TAG, "mdns start");
    ESP_RETURN_ON_ERROR(http_start(), TAG, "http start");

    const esp_timer_create_args_t timer = { .callback = portal_linger_cb, .name = "portal" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer, &s_portal_timer), TAG, "portal timer");

    ESP_LOGI(TAG, "web ui on http://" WIFI_MDNS_HOSTNAME ".local");
    return ESP_OK;
}

esp_err_t wifi_prov_start(wifi_prov_method_t methods)
{
    if ((methods & WIFI_PROV_BLUFI) && !(s_running & WIFI_PROV_BLUFI)) {
        ESP_RETURN_ON_ERROR(blufi_start(), TAG, "blufi start");
        s_running |= WIFI_PROV_BLUFI;
    }
    if ((methods & WIFI_PROV_SOFTAP) && !(s_running & WIFI_PROV_SOFTAP)) {
        ESP_RETURN_ON_ERROR(portal_start(), TAG, "portal start");
        s_running |= WIFI_PROV_SOFTAP;
    }
    return ESP_OK;
}

esp_err_t wifi_prov_stop(void)
{
    if (s_running & WIFI_PROV_SOFTAP) {
        portal_stop();
    }
    if (s_running & WIFI_PROV_BLUFI) {
        blufi_stop();
    }
    s_running = WIFI_PROV_NONE;
    return ESP_OK;
}

wifi_prov_method_t wifi_prov_running(void)
{
    return s_running;
}

bool wifi_is_connected(void)
{
    return s_sta_got_ip;
}

bool wifi_is_bluetooth_connected(void)
{
    return s_ble_connected;
}

bool wifi_has_credentials(void)
{
    wifi_config_t cfg;
    return esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK && cfg.sta.ssid[0] != '\0';
}

esp_err_t wifi_forget(void)
{
    wifi_config_t empty = {0};
    esp_wifi_disconnect();
    return esp_wifi_set_config(WIFI_IF_STA, &empty);
}

void wifi_get_ip_str(char *buf, size_t len)
{
    esp_netif_ip_info_t ip = {0};
    if (s_sta_got_ip) {
        esp_netif_get_ip_info(s_sta_netif, &ip);
    }
    snprintf(buf, len, IPSTR, IP2STR(&ip.ip));
}

const char *wifi_softap_ssid(void)
{
    return s_ap_ssid;
}
