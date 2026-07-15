#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "network_manager.h"
#include "modem_power.h"
#include "cJSON.h"
#include "web_client.h"
#include "sms.h"

static const char *TAG = "main";

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
        .urc_handler = sms_manager_urc_handler, /* must be set before network_manager_init() */
    },
};

/* -------------------------------------------------------------------------
 * SMS forwarding (queue owned by forward_task, decoupled from sms_task so a
 * slow/blocked network doesn't stall SMS parsing or multipart reassembly).
 * ---------------------------------------------------------------------- */

#define SMS_FORWARD_QUEUE_LEN 8
static QueueHandle_t s_sms_forward_queue = NULL;

/* TODO: connect (wifi/cellular via network_manager_connect), POST via
 * web_client, retry/backoff on failure, disconnect if not always-on. */
static void forward_sms(const sms_message_t *sms)
{
    ESP_LOGI(TAG, "forward_sms stub: from=%s date=%s complete=%d parts=%u/%u len=%d",
             sms->sender, sms->date, sms->complete, sms->parts_received, sms->total_parts,
             (int)strlen(sms->message));
}

static void forward_task(void *arg)
{
    (void)arg;
    sms_message_t sms;
    while (1) {
        if (xQueueReceive(s_sms_forward_queue, &sms, portMAX_DELAY) == pdTRUE) {
            forward_sms(&sms);
            free(sms.message);
        }
    }
}

/* Runs on sms_task's context - sms.c frees sms->message right after this
 * returns, so we must deep-copy before queuing, not just take the pointer. */
static void on_sms_received(const sms_message_t *sms, void *ctx)
{
    (void)ctx;
    sms_message_t copy = *sms;
    copy.message = sms->message ? strdup(sms->message) : strdup("");

    if (copy.message == NULL || xQueueSend(s_sms_forward_queue, &copy, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Dropping SMS from %s: queue full or alloc failed", sms->sender);
        free(copy.message);
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    network_manager_init(&cfg);
    network_manager_register_event_cb(on_net_event, NULL);

    s_sms_forward_queue = xQueueCreate(SMS_FORWARD_QUEUE_LEN, sizeof(sms_message_t));
    xTaskCreate(forward_task, "sms_forward", 4096, NULL, 4, NULL);
    sms_manager_init(on_sms_received, NULL);
    sms_manager_check_now();
}