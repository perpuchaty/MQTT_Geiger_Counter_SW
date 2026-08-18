#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * SoftAP / captive portal settings
 * ---------------------------------------------------------------------- */
#define WIFI_AP_SSID_PREFIX   "Geiger"
#define WIFI_AP_PASSWORD      "geiger1234"   /* >= 8 chars, or "" for an open network */
#define WIFI_AP_CHANNEL       1
#define WIFI_AP_MAX_CONN      4
#define WIFI_AP_IP            "192.168.4.1"
#define WIFI_STA_MAX_RETRY    5

/** Provisioning transports, may be combined. */
typedef enum {
    WIFI_PROV_NONE   = 0,
    WIFI_PROV_BLUFI  = (1u << 0),   /* BLE, EspBlufi app */
    WIFI_PROV_SOFTAP = (1u << 1),   /* SoftAP + captive portal */
    WIFI_PROV_BOTH   = WIFI_PROV_BLUFI | WIFI_PROV_SOFTAP,
} wifi_prov_method_t;

/** NVS, netif, event loop and the Wi-Fi driver. Connects with stored credentials if any. */
esp_err_t wifi_prov_init(void);

/** Bring up the selected provisioning transports. */
esp_err_t wifi_prov_start(wifi_prov_method_t methods);

/** Tear down every running transport and go back to plain STA mode. */
esp_err_t wifi_prov_stop(void);

wifi_prov_method_t wifi_prov_running(void);

/** True once the station got an IP address. */
bool wifi_is_connected(void);

/** True when credentials are stored in NVS. */
bool wifi_has_credentials(void);

/** Drop the stored credentials and disconnect. */
esp_err_t wifi_forget(void);

/** Current STA IPv4 address as a string, "0.0.0.0" when not connected. */
void wifi_get_ip_str(char *buf, size_t len);

/** SoftAP SSID, derived from the MAC address. */
const char *wifi_softap_ssid(void);

#ifdef __cplusplus
}
#endif
