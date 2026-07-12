#include "unity.h"
#include "network_manager.h"
#include "web_client.h"
#include "test_common.h"

TEST_CASE("cellular connects", "[cellular]")
{
    network_manager_config_t cfg = test_default_config();
    network_manager_init(&cfg);
    TEST_ASSERT_EQUAL(ESP_OK, network_manager_connect_iface(NET_IF_CELLULAR, 40000));
    network_manager_disconnect();
}

TEST_CASE("cellular GET no-tls", "[cellular]")
{
    network_manager_config_t cfg = test_default_config();
    network_manager_init(&cfg);
    TEST_ASSERT_EQUAL(ESP_OK, network_manager_connect_iface(NET_IF_CELLULAR, 40000));
    char resp[256];
    int status = 0;
    TEST_ASSERT_EQUAL(ESP_OK, http_request("http://example.com", NULL, resp, sizeof(resp), &status));
    TEST_ASSERT_EQUAL(200, status);
    network_manager_disconnect();
}

TEST_CASE("cellular GET tls", "[cellular]")
{
    network_manager_config_t cfg = test_default_config();
    network_manager_init(&cfg);
    TEST_ASSERT_EQUAL(ESP_OK, network_manager_connect_iface(NET_IF_CELLULAR, 40000));
    char resp[256];
    int status = 0;
    TEST_ASSERT_EQUAL(ESP_OK, http_request("https://example.com", NULL, resp, sizeof(resp), &status));
    TEST_ASSERT_EQUAL(200, status);
    network_manager_disconnect();
}