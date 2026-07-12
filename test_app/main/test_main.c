#include "unity.h"
#include "nvs_flash.h"
#include "esp_event.h"

void app_main(void)
{
    nvs_flash_init();
    esp_event_loop_create_default();
    unity_run_menu(); /* interactive: pick tests or "*" to run all, over serial monitor */
}