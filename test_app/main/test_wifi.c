#include "unity.h"
#include "network_manager.h"
#include "web_client.h"
#include "test_common.h"

TEST_CASE("wifi connects", "[wifi]")
{
    network_manager_config_t cfg = test_default_config();
    TEST_ASSERT_EQUAL(ESP_OK, network_manager_init(&cfg));
    TEST_ASSERT_EQUAL(ESP_OK, network_manager_connect_iface(NET_IF_WIFI, 15000));
    TEST_ASSERT_EQUAL(NET_STATE_CONNECTED, network_manager_get_state());
    network_manager_disconnect();
}

TEST_CASE("wifi GET no-tls", "[wifi]")
{
    network_manager_config_t cfg = test_default_config();
    network_manager_init(&cfg);
    TEST_ASSERT_EQUAL(ESP_OK, network_manager_connect_iface(NET_IF_WIFI, 15000));
    char resp[256];
    int status = 0;
    TEST_ASSERT_EQUAL(ESP_OK, http_request("http://example.com", NULL, resp, sizeof(resp), &status));
    TEST_ASSERT_EQUAL(200, status);
    network_manager_disconnect();
}

TEST_CASE("wifi GET tls", "[wifi]")
{
    network_manager_config_t cfg = test_default_config();
    network_manager_init(&cfg);
    TEST_ASSERT_EQUAL(ESP_OK, network_manager_connect_iface(NET_IF_WIFI, 15000));
    char resp[256];
    int status = 0;
    TEST_ASSERT_EQUAL(ESP_OK, http_request("https://example.com", NULL, resp, sizeof(resp), &status));
    TEST_ASSERT_EQUAL(200, status);
    network_manager_disconnect();
}