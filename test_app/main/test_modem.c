#include "unity.h"
#include "network_manager.h"
#include "esp_modem_api.h"
#include "test_common.h"

TEST_CASE("modem powers on and responds to AT", "[modem]")
{
    network_manager_config_t cfg = test_default_config();
    TEST_ASSERT_EQUAL(ESP_OK, network_manager_init(&cfg));
    TEST_ASSERT_EQUAL(ESP_OK, network_manager_modem_lock(pdMS_TO_TICKS(5000)));
    esp_modem_dce_t *dce = network_manager_get_modem_dce();
    TEST_ASSERT_NOT_NULL(dce);
    char resp[64];
    esp_err_t err = esp_modem_at(dce, "AT\r", resp, 3000);
    network_manager_modem_unlock();
    TEST_ASSERT_EQUAL(ESP_OK, err);
}