#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "esp_modem_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Types
 * ---------------------------------------------------------------------- */

typedef enum {
    NET_IF_NONE = 0,
    NET_IF_WIFI,
    NET_IF_CELLULAR,
} network_iface_t;

typedef enum {
    NET_STATE_DISCONNECTED = 0,
    NET_STATE_CONNECTING,
    NET_STATE_CONNECTED,
} network_state_t;

typedef enum {
    NET_EVENT_CONNECTED,
    NET_EVENT_DISCONNECTED,
    NET_EVENT_CONNECT_FAILED,
    NET_EVENT_MODEM_FAULT,      /* modem stopped responding to AT commands */
} network_event_t;

typedef struct {
    network_event_t event;
    network_iface_t iface;
} network_event_data_t;

typedef void (*network_event_cb_t)(const network_event_data_t *data, void *ctx);

/* Board-specific modem power control, implemented by the application.
 * Any of these may be NULL if not needed on a given board. */
typedef void (*modem_power_fn_t)(void);
typedef void (*modem_reset_fn_t)(void);

typedef struct {
    int uart_tx_pin;
    int uart_rx_pin;
    int uart_rts_pin;               /* -1 if unused */
    int uart_cts_pin;               /* -1 if unused */
    uint32_t baud_rate;

    modem_power_fn_t power_on;      /* optional */
    modem_power_fn_t power_off;     /* optional */
    modem_reset_fn_t reset;         /* optional, invoked after repeated connect failures */
    bool power_off_when_idle;       /* if true: power_off() after each session,
                                        power_on() before the next connect attempt */

    const char *apn;
    const char *pap_username;       /* optional, NULL if not used */
    const char *pap_password;       /* optional, NULL if not used */

    uint8_t connect_retry_count;
    uint32_t connect_timeout_ms;

    /* Optional: receives raw unsolicited response lines from the modem
     * (e.g. "+CMTI: ...") whenever it's in command mode. NULL if not needed.
     * Runs in esp_modem's internal context - must be fast, non-blocking, and
     * must not itself issue AT commands (post to a queue instead). Set this
     * before calling network_manager_init(); it can't be changed afterwards. */
    esp_err_t (*urc_handler)(uint8_t *data, size_t len);
} cellular_config_t;

typedef struct {
    const char *ssid;
    const char *password;
    bool always_on;                 /* stay associated permanently vs. disconnect between uses */
    uint32_t connect_timeout_ms;
} wifi_config_net_t;

typedef struct {
    wifi_config_net_t wifi;
    cellular_config_t cellular;
} network_manager_config_t;

/* -------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------- */

/* Initializes wifi (driver stays resident afterwards) and the modem (command
 * mode). Safe to call once at startup. */
esp_err_t network_manager_init(const network_manager_config_t *config);

/* Blocking. Tries wifi first, falls back to cellular on failure/timeout.
 * Returns true if a connection was established. */
bool network_manager_connect(uint32_t timeout_ms);

/* Connects only the specified interface — no fallback. iface must be
 * NET_IF_WIFI or NET_IF_CELLULAR. Disconnects the other iface first if
 * it's currently active. For testing / explicit iface selection. */
esp_err_t network_manager_connect_iface(network_iface_t iface, uint32_t timeout_ms);

/* Tears down the active connection. Wifi: disconnects unless always_on.
 * Cellular: drops PPP and returns the modem to command mode. */
void network_manager_disconnect(void);

network_state_t network_manager_get_state(void);
network_iface_t network_manager_get_active_iface(void);

void network_manager_set_wifi_always_on(bool always_on);

/* Cellular signal quality (RSSI in dBm), or INT32_MIN if unavailable. */
int network_manager_get_signal_quality(void);

/* Returns ESP_ERR_NO_MEM if the subscriber slots (currently 4) are full. */
esp_err_t network_manager_register_event_cb(network_event_cb_t cb, void *ctx);

/* -------------------------------------------------------------------------
 * Shared modem access
 *
 * The modem is owned by this component for the lifetime of the application.
 * Any other module that needs to issue AT commands (independent of cellular
 * data sessions) must acquire the lock first. Acquiring blocks for up to
 * `timeout` if a cellular session currently has the modem in PPP mode.
 * ---------------------------------------------------------------------- */

esp_err_t network_manager_modem_lock(TickType_t timeout);
void network_manager_modem_unlock(void);

/* Valid only while holding the lock. Returns NULL if the modem isn't
 * currently in command mode (e.g. mid-teardown). */
esp_modem_dce_t *network_manager_get_modem_dce(void);

#ifdef __cplusplus
}
#endif