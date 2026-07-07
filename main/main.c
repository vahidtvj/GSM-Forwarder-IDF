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

// static void modem_power_on(void) {
//     gpio_set_level(CONFIG_MODEM_POWERON_PIN, 1);
//     gpio_set_level(CONFIG_MODEM_PWRKEY_PIN, 0);
//     vTaskDelay(pdMS_TO_TICKS(100));
//     gpio_set_level(CONFIG_MODEM_PWRKEY_PIN, 1);
//     vTaskDelay(pdMS_TO_TICKS(1000));
//     gpio_set_level(CONFIG_MODEM_PWRKEY_PIN, 0);
// }

static void modem_power_off(void) {
    gpio_set_level(CONFIG_MODEM_POWERON_PIN, 0);
}

static void on_net_event(const network_event_data_t *e, void *ctx) {
    // e.g. drive a status LED, log, etc.
}


network_manager_config_t cfg = {
    .wifi = {
        .ssid = "aasdasdasd",
        .password = "wrongpass",
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

static const char *TAG = "ip_check";

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

    s_ip_response_len = 0;
    s_ip_response[0] = '\0';

    esp_http_client_config_t config = {
        .url = "http://api.ipify.org",
        .event_handler = http_event_handler,
        // .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Status = %d, IP = %s",
                 esp_http_client_get_status_code(client), s_ip_response);
    } else {
        ESP_LOGE(TAG, "GET failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
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