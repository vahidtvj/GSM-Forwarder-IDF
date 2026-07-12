#include "unity.h"
#include "network_manager.h"
#include "time_sync.h"
#include "test_common.h"

TEST_CASE("time syncs over wifi", "[time]")
{
    network_manager_config_t cfg = test_default_config();
    network_manager_init(&cfg);
    TEST_ASSERT_EQUAL(ESP_OK, network_manager_connect_iface(NET_IF_WIFI, 15000));
    time_sync_reset();
    TEST_ASSERT_TRUE(time_sync_wait(10000));
    network_manager_disconnect();
}