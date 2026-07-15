idf_component_register(
    SRC_DIRS "."
    INCLUDE_DIRS "."
    REQUIRES unity network_manager modem_power web_client
             esp_modem esp_http_client esp-tls mbedtls
             nvs_flash esp_wifi esp_netif esp_event driver lwip
    WHOLE_ARCHIVE
)