#include "unity.h"
#include "network_manager.h"
#include "web_client.h"
#include "test_common.h"

TEST_CASE("time syncs over wifi", "[time]")
{
    network_manager_config_t cfg = test_default_config();
    network_manager_init(&cfg);
    TEST_ASSERT_EQUAL(ESP_OK, network_manager_connect_iface(NET_IF_WIFI, 15000));
    web_client_time_sync_reset();
    TEST_ASSERT_TRUE(web_client_time_sync_wait(10000));
    network_manager_disconnect();
}