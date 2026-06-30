#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Brings up the cellular modem over PPP using esp_modem.
 * Call modem_power_on() BEFORE this, and only after
 * esp_netif_init() / esp_event_loop_create_default().
 *
 * Blocks until either an IP is obtained or CONFIG_MODEM_CONNECT_TIMEOUT_MS
 * (defined locally below) elapses.
 *
 * @return ESP_OK if PPP came up and an IP was assigned.
 */
esp_err_t cellular_conn_start(void);

/** Returns true if the PPP link currently holds a valid IP address. */
bool cellular_conn_is_connected(void);

/** Tears down the PPP session and releases the modem (e.g. before sleep). */
void cellular_conn_stop(void);

#ifdef __cplusplus
}
#endif
