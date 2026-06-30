#include "modem_power.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "modem_power";

static void configure_output_pin(int pin, int level)
{
    if (pin < 0) {
        return;
    }
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, level);
}

void modem_hw_reset(void)
{
#if CONFIG_MODEM_RESET_PIN >= 0
    ESP_LOGI(TAG, "Hardware reset pulse on GPIO%d", CONFIG_MODEM_RESET_PIN);
    configure_output_pin(CONFIG_MODEM_RESET_PIN, CONFIG_MODEM_RESET_LEVEL);
    vTaskDelay(pdMS_TO_TICKS(300));
    gpio_set_level(CONFIG_MODEM_RESET_PIN, !CONFIG_MODEM_RESET_LEVEL);
#else
    ESP_LOGW(TAG, "No reset pin configured, skipping hardware reset");
#endif
}

void modem_power_on(void)
{
    ESP_LOGI(TAG, "Starting modem power-on sequence");

    /* 1. Main power rail (e.g. DC boost / load switch feeding the modem) */
#if CONFIG_MODEM_POWERON_PIN >= 0
    ESP_LOGI(TAG, "Enabling main power on GPIO%d", CONFIG_MODEM_POWERON_PIN);
    configure_output_pin(CONFIG_MODEM_POWERON_PIN, 1);
#endif

    /* 2. Keep modem out of reset (idle level = NOT the active reset level) */
#if CONFIG_MODEM_RESET_PIN >= 0
    configure_output_pin(CONFIG_MODEM_RESET_PIN, !CONFIG_MODEM_RESET_LEVEL);
#endif

    /* 3. DTR: hold LOW so the modem stays in normal (non-sleep) mode and
     * keeps listening on UART. Most SIMCOM/A76xx-class modules treat
     * DTR HIGH as "allow sleep" and DTR LOW as "force awake". If your
     * specific module is inverted, flip this level. */
#if CONFIG_MODEM_DTR_PIN >= 0
    ESP_LOGI(TAG, "Forcing DTR LOW (awake) on GPIO%d", CONFIG_MODEM_DTR_PIN);
    configure_output_pin(CONFIG_MODEM_DTR_PIN, 0);
#endif

    /* 4. PWRKEY pulse — mirrors the Arduino setupDevice() timing:
     *    LOW -> 100ms -> HIGH -> 1000ms -> LOW
     *    (Verify against your A7670 variant's datasheet if it doesn't boot.) */
#if CONFIG_MODEM_PWRKEY_PIN >= 0
    ESP_LOGI(TAG, "Pulsing PWRKEY on GPIO%d", CONFIG_MODEM_PWRKEY_PIN);
    configure_output_pin(CONFIG_MODEM_PWRKEY_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(CONFIG_MODEM_PWRKEY_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(1000));
    gpio_set_level(CONFIG_MODEM_PWRKEY_PIN, 0);
#endif

    /* 5. Give the modem time to boot before sending AT commands */
    ESP_LOGI(TAG, "Waiting for modem boot...");
    vTaskDelay(pdMS_TO_TICKS(3000));
    ESP_LOGI(TAG, "Modem power-on sequence complete");
}