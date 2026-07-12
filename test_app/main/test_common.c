#include "test_common.h"
#include "modem_power.h"
#include "sdkconfig.h"

network_manager_config_t test_default_config(void)
{
    return (network_manager_config_t){
        .wifi = {
            .ssid = CONFIG_TEST_WIFI_SSID,
            .password = CONFIG_TEST_WIFI_PASSWORD,
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
            .power_off = NULL,
            .reset = NULL,
            .power_off_when_idle = false,
            .apn = CONFIG_MODEM_PPP_APN,
            .connect_retry_count = 2,
            .connect_timeout_ms = 30000,
        },
    };
}