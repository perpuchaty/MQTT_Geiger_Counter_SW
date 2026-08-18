#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * SoftAP / captive portal settings (menuconfig: Geiger Counter)
 * ---------------------------------------------------------------------- */
#define WIFI_AP_SSID_PREFIX   CONFIG_GEIGER_AP_SSID_PREFIX  /* SSID is suffixed with the last 3 bytes of the MAC address */
#define WIFI_AP_PASSWORD      CONFIG_GEIGER_AP_PASSWORD     /* >= 8 chars, or "" for an open network */
#define WIFI_AP_CHANNEL       CONFIG_GEIGER_AP_CHANNEL
#define WIFI_AP_MAX_CONN      CONFIG_GEIGER_AP_MAX_CONN
#define WIFI_AP_IP            CONFIG_GEIGER_AP_IP
#define WIFI_STA_MAX_RETRY    CONFIG_GEIGER_STA_MAX_RETRY

/* -------------------------------------------------------------------------
 * mDNS / web UI
 * ---------------------------------------------------------------------- */
#define WIFI_MDNS_HOSTNAME    CONFIG_GEIGER_MDNS_HOSTNAME   /* reachable as <hostname>.local */
#define WIFI_MDNS_INSTANCE    CONFIG_GEIGER_MDNS_INSTANCE
#define WIFI_HTTP_PORT        CONFIG_GEIGER_HTTP_PORT
/** SoftAP is shut down this long after the station gets an IP address, 0 keeps it up. */
#define WIFI_PORTAL_LINGER_MS CONFIG_GEIGER_PORTAL_LINGER_MS

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
