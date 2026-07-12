#include <string.h>
#include <stdio.h>

#include "network_manager.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

#include "esp_modem_api.h"

static const char *TAG = "network_manager";

/* -------------------------------------------------------------------------
 * Module state
 * ---------------------------------------------------------------------- */

static network_manager_config_t s_cfg;
static bool s_initialized = false;

static esp_netif_t *s_wifi_netif = NULL;
static esp_netif_t *s_ppp_netif = NULL;

static esp_modem_dce_t *s_dce = NULL;
static SemaphoreHandle_t s_modem_mutex = NULL;

static EventGroupHandle_t s_net_events = NULL;
#define BIT_WIFI_CONNECTED   BIT0
#define BIT_WIFI_FAIL        BIT1
#define BIT_PPP_CONNECTED    BIT2
#define BIT_PPP_FAIL         BIT3

static volatile network_state_t s_state = NET_STATE_DISCONNECTED;
static volatile network_iface_t s_active_iface = NET_IF_NONE;

#define NETWORK_MANAGER_MAX_SUBSCRIBERS 4

typedef struct {
    network_event_cb_t cb;
    void *ctx;
} event_subscriber_t;

static event_subscriber_t s_subscribers[NETWORK_MANAGER_MAX_SUBSCRIBERS];

static void fire_event(network_event_t event, network_iface_t iface)
{
    network_event_data_t data = { .event = event, .iface = iface };
    for (int i = 0; i < NETWORK_MANAGER_MAX_SUBSCRIBERS; i++) {
        if (s_subscribers[i].cb) {
            s_subscribers[i].cb(&data, s_subscribers[i].ctx);
        }
    }
}

/* -------------------------------------------------------------------------
 * Wifi
 * ---------------------------------------------------------------------- */

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_DISCONNECTED:
            xEventGroupSetBits(s_net_events, BIT_WIFI_FAIL);
            if (s_active_iface == NET_IF_WIFI) {
                s_state = NET_STATE_DISCONNECTED;
                s_active_iface = NET_IF_NONE;
                fire_event(NET_EVENT_DISCONNECTED, NET_IF_WIFI);
                if (s_cfg.wifi.always_on) {
                    esp_wifi_connect();
                }
            }
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_net_events, BIT_WIFI_CONNECTED);
        s_state = NET_STATE_CONNECTED;
        s_active_iface = NET_IF_WIFI;
        fire_event(NET_EVENT_CONNECTED, NET_IF_WIFI);
    }
}

static esp_err_t init_wifi(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    s_wifi_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = { 0 };
    strlcpy((char *)wifi_config.sta.ssid, s_cfg.wifi.ssid ? s_cfg.wifi.ssid : "", sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, s_cfg.wifi.password ? s_cfg.wifi.password : "", sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Driver stays up from here on; connect/disconnect only toggles
     * association, keeping subsequent connects fast. */
    if (s_cfg.wifi.always_on) {
        esp_wifi_connect();
    }
    return ESP_OK;
}

static bool connect_wifi(uint32_t timeout_ms)
{
    xEventGroupClearBits(s_net_events, BIT_WIFI_CONNECTED | BIT_WIFI_FAIL);
    s_state = NET_STATE_CONNECTING;

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        /* Already associated. */
        s_state = NET_STATE_CONNECTED;
        s_active_iface = NET_IF_WIFI;
        return true;
    }

    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(s_net_events, BIT_WIFI_CONNECTED | BIT_WIFI_FAIL,
                                            pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    return (bits & BIT_WIFI_CONNECTED) != 0;
}

static void disconnect_wifi(void)
{
    if (!s_cfg.wifi.always_on) {
        esp_wifi_disconnect();
    }
    if (s_active_iface == NET_IF_WIFI) {
        s_active_iface = NET_IF_NONE;
        s_state = NET_STATE_DISCONNECTED;
    }
}

/* -------------------------------------------------------------------------
 * Cellular / modem
 * ---------------------------------------------------------------------- */

static void ppp_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == NETIF_PPP_STATUS) {
        switch (id) {
        case NETIF_PPP_ERRORUSER:
            xEventGroupSetBits(s_net_events, BIT_PPP_FAIL);
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_PPP_GOT_IP) {
        xEventGroupSetBits(s_net_events, BIT_PPP_CONNECTED);
        s_state = NET_STATE_CONNECTED;
        s_active_iface = NET_IF_CELLULAR;
        fire_event(NET_EVENT_CONNECTED, NET_IF_CELLULAR);
    } else if (base == IP_EVENT && id == IP_EVENT_PPP_LOST_IP) {
        xEventGroupSetBits(s_net_events, BIT_PPP_FAIL);
        if (s_active_iface == NET_IF_CELLULAR) {
            s_state = NET_STATE_DISCONNECTED;
            s_active_iface = NET_IF_NONE;
            fire_event(NET_EVENT_DISCONNECTED, NET_IF_CELLULAR);
        }
    }
}

/* Brings the modem up in command mode. Called once at init, and again after
 * each cellular session ends. */
static esp_err_t modem_enter_command_mode(void)
{
    if (s_dce == NULL) {
        esp_netif_config_t netif_ppp_cfg = ESP_NETIF_DEFAULT_PPP();
        s_ppp_netif = esp_netif_new(&netif_ppp_cfg);

        esp_modem_dte_config_t dte_config = ESP_MODEM_DTE_DEFAULT_CONFIG();
        dte_config.uart_config.tx_io_num = s_cfg.cellular.uart_tx_pin;
        dte_config.uart_config.rx_io_num = s_cfg.cellular.uart_rx_pin;
        dte_config.uart_config.rts_io_num = s_cfg.cellular.uart_rts_pin;
        dte_config.uart_config.cts_io_num = s_cfg.cellular.uart_cts_pin;
        dte_config.uart_config.flow_control = (s_cfg.cellular.uart_rts_pin >= 0 && s_cfg.cellular.uart_cts_pin >= 0)
                                                   ? ESP_MODEM_FLOW_CONTROL_HW
                                                   : ESP_MODEM_FLOW_CONTROL_NONE;
        dte_config.uart_config.baud_rate = s_cfg.cellular.baud_rate;

        ESP_LOGI(TAG, "Creating modem DCE: tx=%d rx=%d rts=%d cts=%d baud=%lu",
                 s_cfg.cellular.uart_tx_pin, s_cfg.cellular.uart_rx_pin,
                 s_cfg.cellular.uart_rts_pin, s_cfg.cellular.uart_cts_pin,
                 (unsigned long)s_cfg.cellular.baud_rate);

        esp_modem_dce_config_t dce_config = ESP_MODEM_DCE_DEFAULT_CONFIG(s_cfg.cellular.apn);

        /* A7670 shares SIMCOM's SIM7600 AT command set closely enough that
         * esp_modem's SIM7600 profile works for it. If you swap to a modem
         * with a materially different command set, change this device type
         * (see esp_modem_dce_device_t in esp_modem_api.h for the full list). */
        s_dce = esp_modem_new_dev(ESP_MODEM_DCE_SIM7600, &dte_config, &dce_config, s_ppp_netif);
        if (s_dce == NULL) {
            ESP_LOGE(TAG, "Failed to create modem DCE");
            return ESP_FAIL;
        }

        if (s_cfg.cellular.urc_handler) {
#ifdef CONFIG_ESP_MODEM_URC_HANDLER
            esp_err_t urc_err = esp_modem_set_urc(s_dce, s_cfg.cellular.urc_handler);
            if (urc_err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to register URC handler: %s", esp_err_to_name(urc_err));
            }
#else
            ESP_LOGW(TAG, "urc_handler set in config, but CONFIG_ESP_MODEM_URC_HANDLER is "
                          "not enabled - run 'idf.py menuconfig' -> Component config -> "
                          "ESP-MODEM -> enable URC handler support");
#endif
        }

        esp_err_t sync_err = esp_modem_sync(s_dce);
        if (sync_err != ESP_OK) {
            ESP_LOGE(TAG, "Modem not responding to AT sync (%s) - check power/wiring/pins",
                     esp_err_to_name(sync_err));
        } else {
            ESP_LOGI(TAG, "Modem responded to AT sync OK");
        }
        return ESP_OK;
    }

    return esp_modem_set_mode(s_dce, ESP_MODEM_MODE_COMMAND);
}

static bool connect_cellular(uint32_t timeout_ms)
{
    if (xSemaphoreTake(s_modem_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return false;
    }

    bool ok = false;
    if (s_cfg.cellular.power_off_when_idle && s_cfg.cellular.power_on) {
        s_cfg.cellular.power_on();
    }

    for (uint8_t attempt = 0; attempt <= s_cfg.cellular.connect_retry_count; attempt++) {
        ESP_LOGI(TAG, "Cellular connect attempt %d/%d", attempt + 1, s_cfg.cellular.connect_retry_count + 1);

        if (modem_enter_command_mode() != ESP_OK) {
            ESP_LOGW(TAG, "Modem failed to enter command mode (attempt %d)", attempt + 1);
            continue;
        }

        if (s_cfg.cellular.pap_username && s_cfg.cellular.pap_password) {
            char cmd[160];
            char resp[64];
            snprintf(cmd, sizeof(cmd), "AT+CGAUTH=1,1,\"%s\",\"%s\"\r",
                     s_cfg.cellular.pap_username, s_cfg.cellular.pap_password);
            if (esp_modem_at(s_dce, cmd, resp, 5000) != ESP_OK) {
                ESP_LOGW(TAG, "Failed to set PPP auth credentials");
            }
        }

        xEventGroupClearBits(s_net_events, BIT_PPP_CONNECTED | BIT_PPP_FAIL);
        s_state = NET_STATE_CONNECTING;

        esp_err_t mode_err = esp_modem_set_mode(s_dce, ESP_MODEM_MODE_DATA);
        if (mode_err != ESP_OK) {
            ESP_LOGW(TAG, "esp_modem_set_mode(DATA) failed: %s (attempt %d) - modem may be unresponsive/unpowered",
                     esp_err_to_name(mode_err), attempt + 1);
            continue;
        }

        EventBits_t bits = xEventGroupWaitBits(s_net_events, BIT_PPP_CONNECTED | BIT_PPP_FAIL,
                                                pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
        if (bits & BIT_PPP_CONNECTED) {
            ok = true;
            break;
        }
        ESP_LOGW(TAG, "PPP did not come up within %lu ms (attempt %d)", (unsigned long)timeout_ms, attempt + 1);

        esp_modem_set_mode(s_dce, ESP_MODEM_MODE_COMMAND);
    }

    if (!ok) {
        fire_event(NET_EVENT_CONNECT_FAILED, NET_IF_CELLULAR);
        if (s_cfg.cellular.reset) {
            s_cfg.cellular.reset();
        }
        if (s_cfg.cellular.power_off_when_idle && s_cfg.cellular.power_off) {
            s_cfg.cellular.power_off();
        }
        xSemaphoreGive(s_modem_mutex);
    }
    /* On success, the mutex is intentionally held until disconnect_cellular()
     * so no other module can issue AT commands while PPP owns the UART. */
    return ok;
}

static void disconnect_cellular(void)
{
    if (s_active_iface != NET_IF_CELLULAR) {
        return;
    }

    esp_modem_set_mode(s_dce, ESP_MODEM_MODE_COMMAND);
    s_state = NET_STATE_DISCONNECTED;
    s_active_iface = NET_IF_NONE;

    if (s_cfg.cellular.power_off_when_idle && s_cfg.cellular.power_off) {
        s_cfg.cellular.power_off();
    }

    xSemaphoreGive(s_modem_mutex);
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

esp_err_t network_manager_init(const network_manager_config_t *config)
{
    if (s_initialized) {
        return ESP_OK;
    }
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg = *config;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    esp_err_t event_loop_err = esp_event_loop_create_default();
    if (event_loop_err != ESP_OK && event_loop_err != ESP_ERR_INVALID_STATE) {
        return event_loop_err;
    }

    s_net_events = xEventGroupCreate();
    s_modem_mutex = xSemaphoreCreateMutex();
    if (s_net_events == NULL || s_modem_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(init_wifi());

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_GOT_IP, &ppp_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_LOST_IP, &ppp_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, &ppp_event_handler, NULL));

    if (s_cfg.cellular.power_on && !s_cfg.cellular.power_off_when_idle) {
        s_cfg.cellular.power_on();
    }
    if (xSemaphoreTake(s_modem_mutex, portMAX_DELAY) == pdTRUE) {
        modem_enter_command_mode();
        xSemaphoreGive(s_modem_mutex);
    }

    s_initialized = true;
    return ESP_OK;
}

bool network_manager_connect(uint32_t timeout_ms)
{
    if (!s_initialized) {
        return false;
    }
    if (s_state == NET_STATE_CONNECTED) {
        return true;
    }

    if (connect_wifi(timeout_ms)) {
        return true;
    }

    return connect_cellular(s_cfg.cellular.connect_timeout_ms
                                 ? s_cfg.cellular.connect_timeout_ms
                                 : timeout_ms);
}

esp_err_t network_manager_connect_iface(network_iface_t iface, uint32_t timeout_ms)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (iface != NET_IF_WIFI && iface != NET_IF_CELLULAR) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_state == NET_STATE_CONNECTED) {
        if (s_active_iface == iface) {
            return ESP_OK;
        }
        network_manager_disconnect();
    }

    bool ok = (iface == NET_IF_WIFI)
                  ? connect_wifi(timeout_ms)
                  : connect_cellular(s_cfg.cellular.connect_timeout_ms
                                          ? s_cfg.cellular.connect_timeout_ms
                                          : timeout_ms);
    return ok ? ESP_OK : ESP_FAIL;
}

void network_manager_disconnect(void)
{
    if (s_active_iface == NET_IF_WIFI) {
        disconnect_wifi();
    } else if (s_active_iface == NET_IF_CELLULAR) {
        disconnect_cellular();
    }
}

network_state_t network_manager_get_state(void)
{
    return s_state;
}

network_iface_t network_manager_get_active_iface(void)
{
    return s_active_iface;
}

void network_manager_set_wifi_always_on(bool always_on)
{
    s_cfg.wifi.always_on = always_on;
    if (always_on) {
        esp_wifi_connect();
    }
}

int network_manager_get_signal_quality(void)
{
    if (s_dce == NULL) {
        return INT32_MIN;
    }
    int rssi = 0, ber = 0;
    if (xSemaphoreTake(s_modem_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return INT32_MIN;
    }
    esp_err_t err = esp_modem_get_signal_quality(s_dce, &rssi, &ber);
    xSemaphoreGive(s_modem_mutex);
    if (err != ESP_OK) {
        return INT32_MIN;
    }
    return rssi;
}

esp_err_t network_manager_register_event_cb(network_event_cb_t cb, void *ctx)
{
    for (int i = 0; i < NETWORK_MANAGER_MAX_SUBSCRIBERS; i++) {
        if (s_subscribers[i].cb == NULL) {
            s_subscribers[i].cb = cb;
            s_subscribers[i].ctx = ctx;
            return ESP_OK;
        }
    }
    ESP_LOGE(TAG, "No free event subscriber slots (max %d)", NETWORK_MANAGER_MAX_SUBSCRIBERS);
    return ESP_ERR_NO_MEM;
}

esp_err_t network_manager_modem_lock(TickType_t timeout)
{
    return xSemaphoreTake(s_modem_mutex, timeout) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

void network_manager_modem_unlock(void)
{
    xSemaphoreGive(s_modem_mutex);
}

esp_modem_dce_t *network_manager_get_modem_dce(void)
{
    return s_dce;
}
