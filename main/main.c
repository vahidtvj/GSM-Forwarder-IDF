#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "network_manager.h"
#include "esp_http_client.h"
#include "modem_power.h"
#include "cJSON.h"
#include "http_client.h"

#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include <time.h>

static const char *TAG = "main";

static bool s_time_synced = false;

static void sync_time_if_needed(void)
{
    if (s_time_synced) {
        return;
    }
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&config);

    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000)) == ESP_OK) {
        time_t now = time(NULL);
        ESP_LOGI(TAG, "Time synced: %s", ctime(&now));
        s_time_synced = true;
    } else {
        ESP_LOGW(TAG, "Time sync failed/timed out - HTTPS cert checks may fail");
    }
}

static void modem_power_off(void) {
    gpio_set_level(CONFIG_MODEM_POWERON_PIN, 0);
}

static void on_net_event(const network_event_data_t *e, void *ctx) {
    // e.g. drive a status LED, log, etc.
}


network_manager_config_t cfg = {
    .wifi = {
        .ssid = CONFIG_WIFI_SSID,
        .password = CONFIG_WIFI_PASSWORD,
        .always_on = false,
        .connect_timeout_ms = 8000,
    },
    .cellular = {
        .uart_tx_pin = CONFIG_MODEM_TX_PIN,
        .uart_rx_pin = CONFIG_MODEM_RX_PIN,
        .uart_rts_pin = -1,
        .uart_cts_pin = -1,
        .baud_rate = CONFIG_MODEM_BAUDRATE,
        .power_on = modem_power_on,
        .power_off = modem_power_off,
        .reset = NULL,
        .power_off_when_idle = false,
        .apn = CONFIG_MODEM_PPP_APN,
        .connect_retry_count = 3,
        .connect_timeout_ms = 30000,
    },
};

static char s_ip_response[64] = {0};
static int  s_ip_response_len = 0;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (s_ip_response_len + evt->data_len < sizeof(s_ip_response)) {
            memcpy(s_ip_response + s_ip_response_len, evt->data, evt->data_len);
            s_ip_response_len += evt->data_len;
            s_ip_response[s_ip_response_len] = '\0';
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}


void check_public_ip(void)
{
    if (!network_manager_connect(15000)) {
        ESP_LOGE(TAG, "No network connection available");
        return;
    }

    sync_time_if_needed(); 

    char resp[64];
    int status = 0;
    esp_err_t err = http_request("https://example.com", NULL, resp, sizeof(resp), &status);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Status = %d, IP = %s", status, resp);
    } else {
        ESP_LOGE(TAG, "GET failed: %s", esp_err_to_name(err));
    }

    network_manager_disconnect();
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Step 1: power-on + independent WiFi / cellular test ===");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    network_manager_init(&cfg);
    network_manager_register_event_cb(on_net_event, NULL);
    check_public_ip();

    // while (1) {
    //     vTaskDelay(pdMS_TO_TICKS(10000));
    //     ESP_LOGI(TAG, "alive — wifi=%d cellular=%d",
    //              wifi_conn_is_connected(), cellular_conn_is_connected());
    // }
}