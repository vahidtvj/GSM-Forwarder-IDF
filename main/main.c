#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "modem_power.h"
#include "wifi_conn.h"
#include "cellular_conn.h"

static const char *TAG = "step1_test";

void app_main(void)
{
    /* Temporary: see raw AT command traffic while debugging cellular bring-up.
     * Wildcard so we don't have to guess esp_modem's internal tag names.
     * Remove / lower once the modem link is confirmed working. */
    esp_log_level_set("*", ESP_LOG_DEBUG);

    ESP_LOGI(TAG, "=== Step 1: power-on + independent WiFi / cellular test ===");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* --- WiFi test --- */
    ESP_LOGI(TAG, "--- Testing WiFi ---");
    esp_err_t wifi_result = wifi_conn_start();
    ESP_LOGI(TAG, "WiFi result: %s", esp_err_to_name(wifi_result));

    /* --- Modem power-on + cellular test --- */
    ESP_LOGI(TAG, "--- Powering on modem ---");
    modem_power_on();

    ESP_LOGI(TAG, "--- Testing cellular (PPP) ---");
    esp_err_t cell_result = cellular_conn_start();
    ESP_LOGI(TAG, "Cellular result: %s", esp_err_to_name(cell_result));

    ESP_LOGI(TAG, "=== Step 1 summary ===");
    ESP_LOGI(TAG, "WiFi:     %s", wifi_conn_is_connected() ? "ONLINE" : "offline");
    ESP_LOGI(TAG, "Cellular: %s", cellular_conn_is_connected() ? "ONLINE" : "offline");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "alive — wifi=%d cellular=%d",
                 wifi_conn_is_connected(), cellular_conn_is_connected());
    }
}