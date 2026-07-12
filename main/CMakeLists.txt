idf_component_register(SRCS  "main.c"
                             "modem_power.c"
                             "http_client.c"
                       INCLUDE_DIRS "."
                       REQUIRES network_manager esp_http_client esp-tls mbedtls
                       PRIV_REQUIRES nvs_flash
                                     mbedtls
                                     esp_wifi
                                     esp_netif
                                     esp_event
                                     esp_driver_gpio
                                     esp_modem
                                     esp_http_client
                                     lwip)