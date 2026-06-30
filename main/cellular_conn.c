#include "cellular_conn.h"
#include "modem_power.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_modem_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "sdkconfig.h"

static const char *TAG = "cellular_conn";

/* Not yet exposed in menuconfig — fixed for Step 1 testing.
 * We can promote this to a Kconfig entry once the PPP link is verified. */
#define MODEM_CONNECT_TIMEOUT_MS 30000
#define MODEM_SYNC_RETRIES 5
#define MODEM_SYNC_RETRY_DELAY_MS 1000
#define MODEM_SIM_SETTLE_MS 5000
#define MODEM_DIAL_RETRIES 4
#define MODEM_DIAL_RETRY_DELAY_MS 4000

#define PPP_CONNECTED_BIT BIT0
#define PPP_FAIL_BIT       BIT1

static EventGroupHandle_t s_ppp_event_group;
static esp_modem_dce_t *s_dce = NULL;
static esp_netif_t *s_ppp_netif = NULL;
static bool s_connected = false;

static void on_ppp_changed(void *arg, esp_event_base_t event_base,
                            int32_t event_id, void *event_data)
{
    ESP_LOGI(TAG, "PPP state changed event %ld", event_id);
    if (event_id == NETIF_PPP_ERRORUSER) {
        ESP_LOGW(TAG, "PPP user error / disconnect");
        s_connected = false;
        xEventGroupSetBits(s_ppp_event_group, PPP_FAIL_BIT);
    }
}

static void on_ip_event(void *arg, esp_event_base_t event_base,
                         int32_t event_id, void *event_data)
{
    if (event_id == IP_EVENT_PPP_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "PPP got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_connected = true;
        xEventGroupSetBits(s_ppp_event_group, PPP_CONNECTED_BIT);
    } else if (event_id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGW(TAG, "PPP lost IP");
        s_connected = false;
        xEventGroupSetBits(s_ppp_event_group, PPP_FAIL_BIT);
    }
}

bool cellular_conn_is_connected(void)
{
    return s_connected;
}

esp_netif_t *cellular_conn_get_netif(void)
{
    return s_ppp_netif;
}

esp_err_t cellular_conn_start(void)
{
    s_ppp_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, &on_ppp_changed, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_GOT_IP, &on_ip_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_LOST_IP, &on_ip_event, NULL));

    esp_netif_config_t netif_ppp_config = ESP_NETIF_DEFAULT_PPP();
    s_ppp_netif = esp_netif_new(&netif_ppp_config);
    assert(s_ppp_netif);

    esp_modem_dte_config_t dte_config = ESP_MODEM_DTE_DEFAULT_CONFIG();
    dte_config.uart_config.tx_io_num = CONFIG_MODEM_TX_PIN;
    dte_config.uart_config.rx_io_num = CONFIG_MODEM_RX_PIN;
    dte_config.uart_config.baud_rate = CONFIG_MODEM_BAUDRATE;
    dte_config.uart_config.flow_control = ESP_MODEM_FLOW_CONTROL_NONE;

    esp_modem_dce_config_t dce_config = ESP_MODEM_DCE_DEFAULT_CONFIG(CONFIG_MODEM_PPP_APN);

    /* NOTE: the A7670 doesn't have a dedicated entry in esp_modem yet.
     * SIM7600 is AT-command compatible with the A76xx series for the basic
     * "connect to network and bring up PPP" flow used here. If AT setup
     * fails on hardware, this is the first thing to revisit. */
    s_dce = esp_modem_new_dev(ESP_MODEM_DCE_SIM7600, &dte_config, &dce_config, s_ppp_netif);
    if (!s_dce) {
        ESP_LOGE(TAG, "Failed to create modem DCE — check UART wiring/pins");
        return ESP_FAIL;
    }

    /* --- Basic AT sync probe, BEFORE attempting PPP ---
     * This isolates "modem not talking on UART at all" (wiring/baud/DTR/
     * power-on timing) from "modem talks, but PPP/network registration
     * failed" (APN/SIM/signal issues). Retry a few times since the modem
     * may still be settling right after boot. */
    esp_err_t sync_err = ESP_FAIL;
    for (int attempt = 1; attempt <= MODEM_SYNC_RETRIES; attempt++) {
        ESP_LOGI(TAG, "AT sync attempt %d/%d...", attempt, MODEM_SYNC_RETRIES);
        sync_err = esp_modem_sync(s_dce);
        if (sync_err == ESP_OK) {
            ESP_LOGI(TAG, "Modem responded to AT — UART link is alive");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(MODEM_SYNC_RETRY_DELAY_MS));
    }
    if (sync_err != ESP_OK) {
        ESP_LOGE(TAG, "Modem never responded to AT after %d attempts.", MODEM_SYNC_RETRIES);
        ESP_LOGE(TAG, "Check: TX/RX not swapped, baud rate matches modem default, "
                       "DTR/PWRKEY/POWERON timing, and that the modem actually powers up "
                       "(listen/feel for it, check current draw).");
        return ESP_FAIL;
    }

#if CONFIG_NEED_SIM_PIN
    bool pin_ok = false;
    if (esp_modem_read_pin(s_dce, &pin_ok) == ESP_OK && !pin_ok) {
        ESP_LOGI(TAG, "Setting SIM PIN");
        if (esp_modem_set_pin(s_dce, CONFIG_SIM_PIN) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set SIM PIN");
            return ESP_FAIL;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif

    /* The modem answers basic AT quickly, but SIM unlock + network
     * attach takes noticeably longer — observed on hardware as an
     * unsolicited "+CPIN: READY" arriving AFTER a too-early dial attempt
     * had already failed with ERROR/NO CARRIER. Give it room to settle,
     * then retry the dial itself since the first attempt commonly loses
     * this race. */
    ESP_LOGI(TAG, "Letting SIM/network attach settle (%dms)...", MODEM_SIM_SETTLE_MS);
    vTaskDelay(pdMS_TO_TICKS(MODEM_SIM_SETTLE_MS));

    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= MODEM_DIAL_RETRIES; attempt++) {
        ESP_LOGI(TAG, "Switching modem to PPP data mode (attempt %d/%d)...",
                 attempt, MODEM_DIAL_RETRIES);
        err = esp_modem_set_mode(s_dce, ESP_MODEM_MODE_DATA);
        if (err == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "Dial attempt %d failed: %s — retrying in %dms",
                 attempt, esp_err_to_name(err), MODEM_DIAL_RETRY_DELAY_MS);
        /* Make sure we're back in command mode before the next attempt */
        esp_modem_set_mode(s_dce, ESP_MODEM_MODE_COMMAND);
        vTaskDelay(pdMS_TO_TICKS(MODEM_DIAL_RETRY_DELAY_MS));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_modem_set_mode(DATA) failed after %d attempts: %s",
                 MODEM_DIAL_RETRIES, esp_err_to_name(err));
        return err;
    }

    EventBits_t bits = xEventGroupWaitBits(s_ppp_event_group,
                                            PPP_CONNECTED_BIT | PPP_FAIL_BIT,
                                            pdFALSE, pdFALSE,
                                            pdMS_TO_TICKS(MODEM_CONNECT_TIMEOUT_MS));

    if (bits & PPP_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Cellular (PPP) connected");
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Cellular connect timed out / failed");
    return ESP_ERR_TIMEOUT;
}

void cellular_conn_stop(void)
{
    if (s_dce) {
        esp_modem_set_mode(s_dce, ESP_MODEM_MODE_COMMAND);
        esp_modem_destroy(s_dce);
        s_dce = NULL;
    }
    if (s_ppp_netif) {
        esp_netif_destroy(s_ppp_netif);
        s_ppp_netif = NULL;
    }
    s_connected = false;
}