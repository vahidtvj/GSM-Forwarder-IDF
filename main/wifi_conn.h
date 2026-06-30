#pragma once
#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initializes WiFi in STA mode and attempts to connect using
 * CONFIG_WIFI_SSID / CONFIG_WIFI_PASSWORD, blocking until either
 * connected (got IP) or CONFIG_WIFI_CONNECT_TIMEOUT_MS elapses.
 *
 * Safe to call once at startup. Must be called after nvs_flash_init(),
 * esp_netif_init(), and esp_event_loop_create_default().
 *
 * @return ESP_OK if an IP address was obtained, ESP_FAIL / ESP_ERR_TIMEOUT otherwise.
 */
esp_err_t wifi_conn_start(void);

/** Returns true if WiFi currently holds a valid IP address. */
bool wifi_conn_is_connected(void);

/** Returns the WiFi STA esp_netif handle (NULL until wifi_conn_start() runs). */
esp_netif_t *wifi_conn_get_netif(void);

#ifdef __cplusplus
}
#endif